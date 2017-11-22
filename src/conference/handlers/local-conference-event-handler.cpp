/*
 * local-conference-event-handler.cpp
 * Copyright (C) 2010-2017 Belledonne Communications SARL
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <ctime>

#include "linphone/utils/utils.h"

#include "chat/chat-room/chat-room-id.h"
#include "conference/local-conference.h"
#include "conference/participant-p.h"
#include "content/content-manager.h"
#include "content/content-type.h"
#include "content/content.h"
#include "core/core-p.h"
#include "db/main-db.h"
#include "event-log/events.h"
#include "local-conference-event-handler-p.h"
#include "logger/logger.h"
#include "object/object-p.h"

// TODO: remove me.
#include "private.h"

// =============================================================================

using namespace std;

LINPHONE_BEGIN_NAMESPACE

using namespace Xsd::ConferenceInfo;

// -----------------------------------------------------------------------------

void LocalConferenceEventHandlerPrivate::notifyFullState (const string &notify, const shared_ptr<ParticipantDevice> &device) {
	notifyParticipantDevice(notify, device);
}

void LocalConferenceEventHandlerPrivate::notifyAllExcept (const string &notify, const shared_ptr<Participant> &exceptParticipant) {
	for (const auto &participant : conf->getParticipants()) {
		if (participant != exceptParticipant)
			notifyParticipant(notify, participant);
	}
}

void LocalConferenceEventHandlerPrivate::notifyAll (const string &notify) {
	for (const auto &participant : conf->getParticipants())
		notifyParticipant(notify, participant);
}

void LocalConferenceEventHandlerPrivate::notifyParticipant (const string &notify, const shared_ptr<Participant> &participant) {
	for (const auto &device : participant->getPrivate()->getDevices())
		notifyParticipantDevice(notify, device);
}

void LocalConferenceEventHandlerPrivate::notifyParticipantDevice (const string &notify, const shared_ptr<ParticipantDevice> &device) {
	if (device->isSubscribedToConferenceEventPackage()) {
		LinphoneEvent *ev = device->getConferenceSubscribeEvent();
		LinphoneContent *content = linphone_core_create_content(ev->lc);
		linphone_content_set_buffer(content, (const uint8_t *)notify.c_str(), strlen(notify.c_str()));
		linphone_event_notify(ev, content);
		linphone_content_unref(content);
	}
}

string LocalConferenceEventHandlerPrivate::createNotify (ConferenceType confInfo, int notifyId, bool isFullState) {
	if (notifyId == -1) {
		lastNotify = lastNotify + 1;
		confInfo.setVersion(lastNotify);
	} else {
		confInfo.setVersion(static_cast<unsigned int>(notifyId));
	}
	confInfo.setState(isFullState ? StateType::full : StateType::partial);
	if (!confInfo.getConferenceDescription()) {
		ConferenceDescriptionType description = ConferenceDescriptionType();
		confInfo.setConferenceDescription(description);
	}

	time_t result = time(nullptr);
	confInfo.getConferenceDescription()->setFreeText(Utils::toString(static_cast<long>(result)));

	stringstream notify;
	Xsd::XmlSchema::NamespaceInfomap map;
	map[""].name = "urn:ietf:params:xml:ns:conference-info";
	serializeConferenceInfo(notify, confInfo, map);
	return notify.str();
}

string LocalConferenceEventHandlerPrivate::createNotifyFullState (int notifyId) {
	string entity = conf->getConferenceAddress().asString();
	string subject = conf->getSubject();
	ConferenceType confInfo = ConferenceType(entity);
	UsersType users;
	ConferenceDescriptionType confDescr = ConferenceDescriptionType();
	confDescr.setSubject(subject);
	confInfo.setUsers(users);
	confInfo.setConferenceDescription((const ConferenceDescriptionType) confDescr);

	for (const auto &participant : conf->getParticipants()) {
		UserType user = UserType();
		UserRolesType roles;
		UserType::EndpointSequence endpoints;
		user.setRoles(roles);
		user.setEndpoint(endpoints);
		user.setEntity(participant->getAddress().asString());
		user.getRoles()->getEntry().push_back(participant->isAdmin() ? "admin" : "participant");
		user.setState(StateType::full);

		for (const auto &device : participant->getPrivate()->getDevices()) {
			const string &gruu = device->getAddress().asString();
			EndpointType endpoint = EndpointType();
			endpoint.setEntity(gruu);
			endpoint.setState(StateType::full);
			user.getEndpoint().push_back(endpoint);
		}

		confInfo.getUsers()->getUser().push_back(user);
	}

	return createNotify(confInfo, notifyId, true);
}

string LocalConferenceEventHandlerPrivate::createNotifyMultipart (int notifyId) {
	list<shared_ptr<EventLog>> events = conf->getCore()->getPrivate()->mainDb->getConferenceNotifiedEvents(
		ChatRoomId(conf->getConferenceAddress(), conf->getConferenceAddress()),
		static_cast<unsigned int>(notifyId)
	);

	list<Content> contents;
	for (const auto &eventLog : events) {
		Content content;
		content.setContentType(ContentType("application","conference-info"));
		string body;
		shared_ptr<ConferenceNotifiedEvent> notifiedEvent = static_pointer_cast<ConferenceNotifiedEvent>(eventLog);
		int eventNotifyId = static_cast<int>(notifiedEvent->getNotifyId());
		switch (eventLog->getType()) {
			case EventLog::Type::ConferenceParticipantAdded: {
				shared_ptr<ConferenceParticipantEvent> addedEvent = static_pointer_cast<ConferenceParticipantEvent>(eventLog);
				body = createNotifyParticipantAdded(
					addedEvent->getParticipantAddress(),
					eventNotifyId
				);
			} break;

			case EventLog::Type::ConferenceParticipantRemoved: {
				shared_ptr<ConferenceParticipantEvent> removedEvent = static_pointer_cast<ConferenceParticipantEvent>(eventLog);
				body = createNotifyParticipantRemoved(
					removedEvent->getParticipantAddress(),
					eventNotifyId
				);
			} break;

			case EventLog::Type::ConferenceParticipantSetAdmin: {
				shared_ptr<ConferenceParticipantEvent> setAdminEvent = static_pointer_cast<ConferenceParticipantEvent>(eventLog);
				body = createNotifyParticipantAdmined(
					setAdminEvent->getParticipantAddress(),
					true,
					eventNotifyId
				);
			} break;

			case EventLog::Type::ConferenceParticipantUnsetAdmin: {
				shared_ptr<ConferenceParticipantEvent> unsetAdminEvent = static_pointer_cast<ConferenceParticipantEvent>(eventLog);
				body = createNotifyParticipantAdmined(
					unsetAdminEvent->getParticipantAddress(),
					false,
					eventNotifyId
				);
			} break;

			case EventLog::Type::ConferenceParticipantDeviceAdded: {
				shared_ptr<ConferenceParticipantDeviceEvent> deviceAddedEvent = static_pointer_cast<ConferenceParticipantDeviceEvent>(eventLog);
				body = createNotifyParticipantDeviceAdded(
					deviceAddedEvent->getParticipantAddress(),
					deviceAddedEvent->getDeviceAddress(),
					eventNotifyId
				);
			} break;

			case EventLog::Type::ConferenceParticipantDeviceRemoved: {
				shared_ptr<ConferenceParticipantDeviceEvent> deviceRemovedEvent = static_pointer_cast<ConferenceParticipantDeviceEvent>(eventLog);
				body = createNotifyParticipantDeviceRemoved(
					deviceRemovedEvent->getParticipantAddress(),
					deviceRemovedEvent->getDeviceAddress(),
					eventNotifyId
				);
			} break;

			case EventLog::Type::ConferenceSubjectChanged: {
				shared_ptr<ConferenceSubjectEvent> subjectEvent = static_pointer_cast<ConferenceSubjectEvent>(eventLog);
				body = createNotifySubjectChanged(
					subjectEvent->getSubject(),
					eventNotifyId
				);
			} break;

			default:
				// We should never pass here!
				L_ASSERT(false);
				continue;
		}
		content.setBody(body);
		contents.push_back(content);
	}

	ContentManager contentManager;
	Content multipart = contentManager.contentsListToMultipart(contents);
	return multipart.getBodyAsString();
}

string LocalConferenceEventHandlerPrivate::createNotifyParticipantAdded (const Address &addr, int notifyId) {
	string entity = conf->getConferenceAddress().asString();
	ConferenceType confInfo = ConferenceType(entity);
	UsersType users;
	confInfo.setUsers(users);
	UserType user = UserType();
	UserRolesType roles;
	UserType::EndpointSequence endpoints;

	shared_ptr<Participant> p = conf->findParticipant(addr);
	if (p) {
		for (const auto &device : p->getPrivate()->getDevices()) {
			const string &gruu = device->getAddress().asString();
			EndpointType endpoint = EndpointType();
			endpoint.setEntity(gruu);
			endpoint.setState(StateType::full);
			user.getEndpoint().push_back(endpoint);
		}
	}

	user.setRoles(roles);
	user.setEntity(addr.asStringUriOnly());
	user.getRoles()->getEntry().push_back("participant");
	user.setState(StateType::full);

	confInfo.getUsers()->getUser().push_back(user);

	return createNotify(confInfo, notifyId);
}

string LocalConferenceEventHandlerPrivate::createNotifyParticipantRemoved (const Address &addr, int notifyId) {
	string entity = conf->getConferenceAddress().asString();
	ConferenceType confInfo = ConferenceType(entity);
	UsersType users;
	confInfo.setUsers(users);

	UserType user = UserType();
	user.setEntity(addr.asStringUriOnly());
	user.setState(StateType::deleted);
	confInfo.getUsers()->getUser().push_back(user);

	return createNotify(confInfo, notifyId);
}

string LocalConferenceEventHandlerPrivate::createNotifyParticipantAdmined (const Address &addr, bool isAdmin, int notifyId) {
	string entity = conf->getConferenceAddress().asString();
	ConferenceType confInfo = ConferenceType(entity);
	UsersType users;
	confInfo.setUsers(users);

	UserType user = UserType();
	UserRolesType roles;
	user.setRoles(roles);
	user.setEntity(addr.asStringUriOnly());
	user.getRoles()->getEntry().push_back(isAdmin ? "admin" : "participant");
	user.setState(StateType::partial);
	confInfo.getUsers()->getUser().push_back(user);

	return createNotify(confInfo, notifyId);
}

string LocalConferenceEventHandlerPrivate::createNotifySubjectChanged (int notifyId) {
	return createNotifySubjectChanged(conf->getSubject(), notifyId);
}

string LocalConferenceEventHandlerPrivate::createNotifySubjectChanged (const string &subject, int notifyId) {
	string entity = conf->getConferenceAddress().asString();
	ConferenceType confInfo = ConferenceType(entity);
	ConferenceDescriptionType confDescr = ConferenceDescriptionType();
	confDescr.setSubject(subject);
	confInfo.setConferenceDescription((const ConferenceDescriptionType)confDescr);

	return createNotify(confInfo, notifyId);
}

string LocalConferenceEventHandlerPrivate::createNotifyParticipantDeviceAdded (const Address &addr, const Address &gruu, int notifyId) {
	string entity = conf->getConferenceAddress().asString();
	ConferenceType confInfo = ConferenceType(entity);
	UsersType users;
	confInfo.setUsers(users);

	UserType user = UserType();
	UserRolesType roles;
	UserType::EndpointSequence endpoints;
	user.setRoles(roles);
	user.setEntity(addr.asStringUriOnly());
	user.getRoles()->getEntry().push_back("participant");
	user.setState(StateType::partial);

	EndpointType endpoint = EndpointType();
	endpoint.setEntity(gruu.asStringUriOnly());
	endpoint.setState(StateType::full);
	user.getEndpoint().push_back(endpoint);

	confInfo.getUsers()->getUser().push_back(user);

	return createNotify(confInfo, notifyId);
}

string LocalConferenceEventHandlerPrivate::createNotifyParticipantDeviceRemoved (const Address &addr, const Address &gruu, int notifyId) {
	string entity = conf->getConferenceAddress().asString();
	ConferenceType confInfo = ConferenceType(entity);
	UsersType users;
	confInfo.setUsers(users);

	UserType user = UserType();
	UserRolesType roles;
	UserType::EndpointSequence endpoints;
	user.setRoles(roles);
	user.setEntity(addr.asStringUriOnly());
	user.getRoles()->getEntry().push_back("participant");
	user.setState(StateType::partial);

	EndpointType endpoint = EndpointType();
	endpoint.setEntity(gruu.asStringUriOnly());
	endpoint.setState(StateType::deleted);
	user.getEndpoint().push_back(endpoint);

	confInfo.getUsers()->getUser().push_back(user);

	return createNotify(confInfo, notifyId);
}

// =============================================================================

LocalConferenceEventHandler::LocalConferenceEventHandler (LocalConference *localConference) :
	Object(*new LocalConferenceEventHandlerPrivate) {
	L_D();
	xercesc::XMLPlatformUtils::Initialize();
	d->conf = localConference;
	// TODO : init d->lastNotify = last notify
}

LocalConferenceEventHandler::~LocalConferenceEventHandler () {
	xercesc::XMLPlatformUtils::Terminate();
}

// -----------------------------------------------------------------------------

void LocalConferenceEventHandler::subscribeReceived (LinphoneEvent *lev) {
	L_D();
	const LinphoneAddress *lAddr = linphone_event_get_from(lev);
	char *addrStr = linphone_address_as_string(lAddr);
	shared_ptr<Participant> participant = d->conf->findParticipant(Address(addrStr));
	bctbx_free(addrStr);
	if (!participant)
		return;

	const LinphoneAddress *lContactAddr = linphone_event_get_remote_contact(lev);
	char *contactAddrStr = linphone_address_as_string(lContactAddr);
	Address contactAddr(contactAddrStr);
	bctbx_free(contactAddrStr);
	if (contactAddr.getUriParamValue("gr").empty())
		return;
	IdentityAddress gruu(contactAddr);
	shared_ptr<ParticipantDevice> device = participant->getPrivate()->addDevice(gruu);

	if (linphone_event_get_subscription_state(lev) == LinphoneSubscriptionActive) {
		unsigned int lastNotify = static_cast<unsigned int>(Utils::stoi(linphone_event_get_custom_header(lev, "Last-Notify-Version")));
		if (lastNotify == 0) {
			lInfo() << "Sending initial notify of conference:" << d->conf->getConferenceAddress().asString() << " to: " << device->getAddress().asString();
			device->setConferenceSubscribeEvent(lev);
			d->notifyFullState(d->createNotifyFullState(), device);
		} else if (lastNotify < d->lastNotify) {
			lInfo() << "Sending all missed notify for conference:" << d->conf->getConferenceAddress().asString() <<
				" from: " << lastNotify << " to: " << participant->getAddress().asString();
			d->notifyParticipantDevice(d->createNotifyMultipart(static_cast<int>(lastNotify)), device);
		} else if (lastNotify > d->lastNotify) {
			lError() << "last notify received by client: [" << lastNotify <<"] for conference:" <<
				d->conf->getConferenceAddress().asString() <<
				" should not be higher than last notify sent by server: [" << d->lastNotify << "]";
		}
	} else if (linphone_event_get_subscription_state(lev) == LinphoneSubscriptionTerminated)
		device->setConferenceSubscribeEvent(nullptr);
}

void LocalConferenceEventHandler::notifyParticipantAdded (const Address &addr) {
	L_D();
	shared_ptr<Participant> participant = d->conf->findParticipant(addr);
	d->notifyAllExcept(d->createNotifyParticipantAdded(addr), participant);
}

void LocalConferenceEventHandler::notifyParticipantRemoved (const Address &addr) {
	L_D();
	shared_ptr<Participant> participant = d->conf->findParticipant(addr);
	d->notifyAllExcept(d->createNotifyParticipantRemoved(addr), participant);
}

void LocalConferenceEventHandler::notifyParticipantSetAdmin (const Address &addr, bool isAdmin) {
	L_D();
	d->notifyAll(d->createNotifyParticipantAdmined(addr, isAdmin));
}

void LocalConferenceEventHandler::notifySubjectChanged () {
	L_D();
	d->notifyAll(d->createNotifySubjectChanged());
}

void LocalConferenceEventHandler::notifyParticipantDeviceAdded (const Address &addr, const Address &gruu) {
	L_D();
	d->notifyAll(d->createNotifyParticipantDeviceAdded(addr, gruu));
}

void LocalConferenceEventHandler::notifyParticipantDeviceRemoved (const Address &addr, const Address &gruu) {
	L_D();
	d->notifyAll(d->createNotifyParticipantDeviceRemoved(addr, gruu));
}

LINPHONE_END_NAMESPACE