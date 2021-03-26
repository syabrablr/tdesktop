/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "payments/payments_checkout_process.h"

#include "payments/payments_form.h"
#include "payments/ui/payments_panel.h"
#include "main/main_session.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "storage/storage_domain.h"
#include "history/history_item.h"
#include "history/history.h"
#include "data/data_user.h" // UserData::isBot.
#include "core/local_url_handlers.h" // TryConvertUrlToLocal.
#include "core/file_utilities.h" // File::OpenUrl.
#include "apiwrap.h"

// #TODO payments errors
#include "mainwindow.h"
#include "ui/toasts/common_toasts.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>

namespace Payments {
namespace {

struct SessionProcesses {
	base::flat_map<FullMsgId, std::unique_ptr<CheckoutProcess>> map;
	rpl::lifetime lifetime;
};

base::flat_map<not_null<Main::Session*>, SessionProcesses> Processes;

[[nodiscard]] SessionProcesses &LookupSessionProcesses(
		not_null<const HistoryItem*> item) {
	const auto session = &item->history()->session();
	const auto i = Processes.find(session);
	if (i != end(Processes)) {
		return i->second;
	}
	const auto j = Processes.emplace(session).first;
	auto &result = j->second;
	session->account().sessionChanges(
	) | rpl::start_with_next([=] {
		Processes.erase(session);
	}, result.lifetime);
	return result;
}

} // namespace

void CheckoutProcess::Start(not_null<const HistoryItem*> item) {
	auto &processes = LookupSessionProcesses(item);
	const auto session = &item->history()->session();
	const auto id = item->fullId();
	const auto i = processes.map.find(id);
	if (i != end(processes.map)) {
		i->second->requestActivate();
		return;
	}
	const auto j = processes.map.emplace(
		id,
		std::make_unique<CheckoutProcess>(session, id, PrivateTag{})).first;
	j->second->requestActivate();
}

CheckoutProcess::CheckoutProcess(
	not_null<Main::Session*> session,
	FullMsgId itemId,
	PrivateTag)
: _session(session)
, _form(std::make_unique<Form>(session, itemId))
, _panel(std::make_unique<Ui::Panel>(panelDelegate())) {
	_form->updates(
	) | rpl::start_with_next([=](const FormUpdate &update) {
		handleFormUpdate(update);
	}, _lifetime);
	_panel->backRequests(
	) | rpl::start_with_next([=] {
		showForm();
	}, _panel->lifetime());
}

CheckoutProcess::~CheckoutProcess() {
}

void CheckoutProcess::requestActivate() {
	_panel->requestActivate();
}

not_null<Ui::PanelDelegate*> CheckoutProcess::panelDelegate() {
	return static_cast<PanelDelegate*>(this);
}

void CheckoutProcess::handleFormUpdate(const FormUpdate &update) {
	v::match(update, [&](const FormReady &) {
		performInitialSilentValidation();
		if (!_initialSilentValidation) {
			showForm();
		}
	}, [&](const ThumbnailUpdated &data) {
		_panel->updateFormThumbnail(data.thumbnail);
	}, [&](const ValidateFinished &) {
		if (_initialSilentValidation) {
			_initialSilentValidation = false;
		}
		showForm();
		if (_submitState == SubmitState::Validation) {
			_submitState = SubmitState::Validated;
			panelSubmit();
		}
	}, [&](const PaymentMethodUpdate &) {
		showForm();
	}, [&](const VerificationNeeded &data) {
		if (!_panel->showWebview(data.url, false)) {
			File::OpenUrl(data.url);
			panelCloseSure();
		}
	}, [&](const PaymentFinished &data) {
		const auto weak = base::make_weak(this);
		_session->api().applyUpdates(data.updates);
		if (weak) {
			panelCloseSure();
		}
	}, [&](const Error &error) {
		handleError(error);
	});
}

void CheckoutProcess::handleError(const Error &error) {
	const auto showToast = [&](const TextWithEntities &text) {
		if (_panel) {
			_panel->requestActivate();
			_panel->showToast(text);
		} else {
			App::wnd()->activate();
			Ui::ShowMultilineToast({ .text = text });
		}
	};
	const auto &id = error.id;
	switch (error.type) {
	case Error::Type::Form:
		if (id == u"INVOICE_ALREADY_PAID"_q) {
			showToast({ "Already Paid!" }); // #TODO payments errors message
		} else if (true
			|| id == u"PROVIDER_ACCOUNT_INVALID"_q
			|| id == u"PROVIDER_ACCOUNT_TIMEOUT"_q) {
			showToast({ "Error: " + id });
		}
		break;
	case Error::Type::Validate:
		if (_submitState == SubmitState::Validation) {
			_submitState = SubmitState::None;
		}
		if (_initialSilentValidation) {
			_initialSilentValidation = false;
			showForm();
			return;
		}
		using Field = Ui::InformationField;
		if (id == u"REQ_INFO_NAME_INVALID"_q) {
			showInformationError(Field::Name);
		} else if (id == u"REQ_INFO_EMAIL_INVALID"_q) {
			showInformationError(Field::Email);
		} else if (id == u"REQ_INFO_PHONE_INVALID"_q) {
			showInformationError(Field::Phone);
		} else if (id == u"ADDRESS_STREET_LINE1_INVALID"_q) {
			showInformationError(Field::ShippingStreet);
		} else if (id == u"ADDRESS_CITY_INVALID"_q) {
			showInformationError(Field::ShippingCity);
		} else if (id == u"ADDRESS_STATE_INVALID"_q) {
			showInformationError(Field::ShippingState);
		} else if (id == u"ADDRESS_COUNTRY_INVALID"_q) {
			showInformationError(Field::ShippingCountry);
		} else if (id == u"ADDRESS_POSTCODE_INVALID"_q) {
			showInformationError(Field::ShippingPostcode);
		} else if (id == u"SHIPPING_BOT_TIMEOUT"_q) {
			showToast({ "Error: Bot Timeout!" }); // #TODO payments errors message
		} else if (id == u"SHIPPING_NOT_AVAILABLE"_q) {
			showToast({ "Error: Shipping to the selected country is not available!" }); // #TODO payments errors message
		} else {
			showToast({ "Error: " + id });
		}
		break;
	case Error::Type::Send:
		if (_submitState == SubmitState::Finishing) {
			_submitState = SubmitState::None;
		}
		if (id == u"PAYMENT_FAILED"_q) {
			showToast({ "Error: Payment Failed. Your card has not been billed." }); // #TODO payments errors message
		} else if (id == u"BOT_PRECHECKOUT_FAILED"_q) {
			showToast({ "Error: PreCheckout Failed. Your card has not been billed." }); // #TODO payments errors message
		} else if (id == u"INVOICE_ALREADY_PAID"_q) {
			showToast({ "Already Paid!" }); // #TODO payments errors message
		} else if (id == u"REQUESTED_INFO_INVALID"_q
			|| id == u"SHIPPING_OPTION_INVALID"_q
			|| id == u"PAYMENT_CREDENTIALS_INVALID"_q
			|| id == u"PAYMENT_CREDENTIALS_ID_INVALID"_q) {
			showToast({ "Error: " + id + ". Your card has not been billed." });
		}
		break;
	default: Unexpected("Error type in CheckoutProcess::handleError.");
	}
}

void CheckoutProcess::panelRequestClose() {
	panelCloseSure(); // #TODO payments confirmation
}

void CheckoutProcess::panelCloseSure() {
	const auto i = Processes.find(_session);
	if (i == end(Processes)) {
		return;
	}
	const auto j = ranges::find(i->second.map, this, [](const auto &pair) {
		return pair.second.get();
	});
	if (j == end(i->second.map)) {
		return;
	}
	i->second.map.erase(j);
	if (i->second.map.empty()) {
		Processes.erase(i);
	}
}

void CheckoutProcess::panelSubmit() {
	if (_submitState == SubmitState::Validation
		|| _submitState == SubmitState::Finishing) {
		return;
	}
	const auto &method = _form->paymentMethod();
	const auto &invoice = _form->invoice();
	const auto &options = _form->shippingOptions();
	if (!options.list.empty() && options.selectedId.isEmpty()) {
		chooseShippingOption();
		return;
	} else if (_submitState != SubmitState::Validated
		&& options.list.empty()
		&& (invoice.isShippingAddressRequested
			|| invoice.isNameRequested
			|| invoice.isEmailRequested
			|| invoice.isPhoneRequested)) {
		_submitState = SubmitState::Validation;
		_form->validateInformation(_form->savedInformation());
		return;
	} else if (!method.newCredentials && !method.savedCredentials) {
		editPaymentMethod();
		return;
	}
	_submitState = SubmitState::Finishing;
	_form->submit();
}

void CheckoutProcess::panelWebviewMessage(const QJsonDocument &message) {
	if (!message.isArray()) {
		LOG(("Payments Error: "
			"Not an array received in buy_callback arguments."));
		return;
	}
	const auto list = message.array();
	if (list.at(0).toString() != "payment_form_submit") {
		return;
	} else if (!list.at(1).isString()) {
		LOG(("Payments Error: "
			"Not a string received in buy_callback result."));
		return;
	}

	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(
		list.at(1).toString().toUtf8(),
		&error);
	if (error.error != QJsonParseError::NoError) {
		LOG(("Payments Error: "
			"Failed to parse buy_callback arguments, error: %1."
			).arg(error.errorString()));
		return;
	} else if (!document.isObject()) {
		LOG(("Payments Error: "
			"Not an object decoded in buy_callback result."));
		return;
	}
	const auto root = document.object();
	const auto title = root.value("title").toString();
	const auto credentials = root.value("credentials");
	if (!credentials.isObject()) {
		LOG(("Payments Error: "
			"Not an object received in payment credentials."));
		return;
	}
	crl::on_main(this, [=] {
		_form->setPaymentCredentials(NewCredentials{
			.title = title,
			.data = QJsonDocument(
				credentials.toObject()
			).toJson(QJsonDocument::Compact),
			.saveOnServer = false, // #TODO payments save
		});
	});
}

bool CheckoutProcess::panelWebviewNavigationAttempt(const QString &uri) {
	if (Core::TryConvertUrlToLocal(uri) == uri) {
		return true;
	}
	crl::on_main(this, [=] {
		panelCloseSure();
		App::wnd()->activate();
	});
	return false;
}

void CheckoutProcess::panelEditPaymentMethod() {
	if (_submitState != SubmitState::None
		&& _submitState != SubmitState::Validated) {
		return;
	}
	editPaymentMethod();
}

void CheckoutProcess::panelValidateCard(Ui::UncheckedCardDetails data) {
	_form->validateCard(data);
}

void CheckoutProcess::panelEditShippingInformation() {
	showEditInformation(Ui::InformationField::ShippingStreet);
}

void CheckoutProcess::panelEditName() {
	showEditInformation(Ui::InformationField::Name);
}

void CheckoutProcess::panelEditEmail() {
	showEditInformation(Ui::InformationField::Email);
}

void CheckoutProcess::panelEditPhone() {
	showEditInformation(Ui::InformationField::Phone);
}

void CheckoutProcess::showForm() {
	_panel->showForm(
		_form->invoice(),
		_form->savedInformation(),
		_form->paymentMethod().ui,
		_form->shippingOptions());
}

void CheckoutProcess::showEditInformation(Ui::InformationField field) {
	if (_submitState != SubmitState::None) {
		return;
	}
	_panel->showEditInformation(
		_form->invoice(),
		_form->savedInformation(),
		field);
}

void CheckoutProcess::showInformationError(Ui::InformationField field) {
	if (_submitState != SubmitState::None) {
		return;
	}
	_panel->showInformationError(
		_form->invoice(),
		_form->savedInformation(),
		field);
}

void CheckoutProcess::chooseShippingOption() {
	_panel->chooseShippingOption(_form->shippingOptions());
}

void CheckoutProcess::editPaymentMethod() {
	_panel->choosePaymentMethod(_form->paymentMethod().ui);
}

void CheckoutProcess::panelChooseShippingOption() {
	if (_submitState != SubmitState::None) {
		return;
	}
	chooseShippingOption();
}

void CheckoutProcess::panelChangeShippingOption(const QString &id) {
	_form->setShippingOption(id);
	showForm();
}

void CheckoutProcess::panelValidateInformation(
		Ui::RequestedInformation data) {
	_form->validateInformation(data);
}

void CheckoutProcess::panelShowBox(object_ptr<Ui::BoxContent> box) {
	_panel->showBox(std::move(box));
}

void CheckoutProcess::performInitialSilentValidation() {
	const auto &invoice = _form->invoice();
	const auto &saved = _form->savedInformation();
	if ((invoice.isNameRequested && saved.name.isEmpty())
		|| (invoice.isEmailRequested && saved.email.isEmpty())
		|| (invoice.isPhoneRequested && saved.phone.isEmpty())
		|| (invoice.isShippingAddressRequested && !saved.shippingAddress)) {
		return;
	}
	_initialSilentValidation = true;
	_form->validateInformation(saved);
}

QString CheckoutProcess::panelWebviewDataPath() {
	return _session->domain().local().webviewDataPath();
}

} // namespace Payments