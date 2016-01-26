// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdlib.h>
#ifdef _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "link.h"
#include "session.h"
#include "amqpvalue.h"
#include "amqp_definitions.h"
#include "amqpalloc.h"
#include "amqp_frame_codec.h"
#include "consolelogger.h"
#include "logger.h"
#include "list.h"

#define DEFAULT_LINK_CREDIT 10000

typedef struct DELIVERY_INSTANCE_TAG
{
	delivery_number delivery_id;
	ON_DELIVERY_SETTLED on_delivery_settled;
	void* callback_context;
	void* link;
} DELIVERY_INSTANCE;

typedef struct LINK_INSTANCE_TAG
{
	SESSION_HANDLE session;
	LINK_STATE link_state;
	LINK_STATE previous_link_state;
	AMQP_VALUE source;
	AMQP_VALUE target;
	handle handle;
	LINK_ENDPOINT_HANDLE link_endpoint;
	char* name;
	LIST_HANDLE pending_deliveries;
	sequence_no delivery_count;
	role role;
	ON_LINK_STATE_CHANGED on_link_state_changed;
	ON_LINK_FLOW_ON on_link_flow_on;
    ON_TRANSFER_RECEIVED on_transfer_received;
	void* callback_context;
	sender_settle_mode snd_settle_mode;
	receiver_settle_mode rcv_settle_mode;
	sequence_no initial_delivery_count;
	uint64_t max_message_size;
	uint32_t link_credit;
	uint32_t available;
    fields attach_properties;
	unsigned char is_underlying_session_begun : 1;
} LINK_INSTANCE;

static void set_link_state(LINK_INSTANCE* link_instance, LINK_STATE link_state)
{
	link_instance->previous_link_state = link_instance->link_state;
	link_instance->link_state = link_state;

	if (link_instance->on_link_state_changed != NULL)
	{
		link_instance->on_link_state_changed(link_instance->callback_context, link_state, link_instance->previous_link_state);
	}
}

static int send_flow(LINK_INSTANCE* link)
{
	int result;
	FLOW_HANDLE flow = flow_create(0, 0, 0);

	if (flow == NULL)
	{
		result = __LINE__;
	}
	else
	{
		if ((flow_set_link_credit(flow, link->link_credit) != 0) ||
			(flow_set_handle(flow, link->handle) != 0) ||
			(flow_set_delivery_count(flow, link->delivery_count) != 0))
		{
			result = __LINE__;
		}
		else
		{
			if (session_send_flow(link->link_endpoint, flow) != 0)
			{
				result = __LINE__;
			}
			else
			{
				result = 0;
			}
		}

		flow_destroy(flow);
	}

	return result;
}

static int send_disposition(LINK_INSTANCE* link_instance, delivery_number delivery_number, AMQP_VALUE delivery_state)
{
	int result;

	DISPOSITION_HANDLE disposition = disposition_create(link_instance->role, delivery_number);
	if (disposition == NULL)
	{
		result = __LINE__;
	}
	else
	{
		if ((disposition_set_last(disposition, delivery_number) != 0) ||
			(disposition_set_settled(disposition, true) != 0) ||
			((delivery_state != NULL) && (disposition_set_state(disposition, delivery_state) != 0)))
		{
			result = __LINE__;
		}
		else
		{
			if (session_send_disposition(link_instance->link_endpoint, disposition) != 0)
			{
				result = __LINE__;
			}
			else
			{
				result = 0;
			}
		}

		disposition_destroy(disposition);
	}

	return result;
}

static int send_detach_frame(LINK_INSTANCE* link_instance, ERROR_HANDLE error_handle)
{
	int result;
	DETACH_HANDLE detach_performative;

	detach_performative = detach_create(0);
	if (detach_performative == NULL)
	{
		result = __LINE__;
	}
	else
	{
		if ((error_handle != NULL) &&
			(detach_set_error(detach_performative, error_handle) != 0))
		{
			result = __LINE__;
		}
		else
		{
			if (session_send_detach(link_instance->link_endpoint, detach_performative) != 0)
			{
				result = __LINE__;
			}
			else
			{
				result = 0;
			}
		}

		detach_destroy(detach_performative);
	}

	return result;
}

static void link_frame_received(void* context, AMQP_VALUE performative, uint32_t payload_size, const unsigned char* payload_bytes)
{
	LINK_INSTANCE* link_instance = (LINK_INSTANCE*)context;
	AMQP_VALUE descriptor = amqpvalue_get_inplace_descriptor(performative);

	if (is_attach_type_by_descriptor(descriptor))
	{
		ATTACH_HANDLE attach_handle;
		if (amqpvalue_get_attach(performative, &attach_handle) == 0)
		{
			if ((link_instance->role == role_receiver) &&
				(attach_get_initial_delivery_count(attach_handle, &link_instance->delivery_count) != 0))
			{
				/* error */
				set_link_state(link_instance, LINK_STATE_DETACHED);
			}
			else
			{
				if (link_instance->link_state == LINK_STATE_HALF_ATTACHED)
				{
					if (link_instance->role == role_receiver)
					{
						link_instance->link_credit = DEFAULT_LINK_CREDIT;
						send_flow(link_instance);
					}
					else
					{
						link_instance->link_credit = 0;
					}

					set_link_state(link_instance, LINK_STATE_ATTACHED);
				}
			}

			attach_destroy(attach_handle);
		}
	}
	else if (is_flow_type_by_descriptor(descriptor))
	{
		FLOW_HANDLE flow_handle;
		if (amqpvalue_get_flow(performative, &flow_handle) == 0)
		{
			if (link_instance->role == role_sender)
			{
				delivery_number rcv_delivery_count;
				uint32_t rcv_link_credit;

				if ((flow_get_link_credit(flow_handle, &rcv_link_credit) != 0) ||
					(flow_get_delivery_count(flow_handle, &rcv_delivery_count) != 0))
				{
					/* error */
					set_link_state(link_instance, LINK_STATE_DETACHED);
				}
				else
				{
					link_instance->link_credit = rcv_delivery_count + rcv_link_credit - link_instance->delivery_count;
					if (link_instance->link_credit > 0)
					{
						link_instance->on_link_flow_on(link_instance->callback_context);
					}
				}
			}
		}

		flow_destroy(flow_handle);
	}
	else if (is_transfer_type_by_descriptor(descriptor))
	{
		if (link_instance->on_transfer_received != NULL)
		{
			TRANSFER_HANDLE transfer_handle;
			if (amqpvalue_get_transfer(performative, &transfer_handle) == 0)
			{
				AMQP_VALUE delivery_state;
				delivery_number received_delivery_id;

				link_instance->link_credit--;
				link_instance->delivery_count++;
				if (link_instance->link_credit == 0)
				{
					link_instance->link_credit = DEFAULT_LINK_CREDIT;
					send_flow(link_instance);
				}

				if (transfer_get_delivery_id(transfer_handle, &received_delivery_id) != 0)
				{
					/* error */
				}
				else
				{
					delivery_state = link_instance->on_transfer_received(link_instance->callback_context, transfer_handle, payload_size, payload_bytes);

					if (send_disposition(link_instance, received_delivery_id, delivery_state) != 0)
					{
						/* error */
					}

					if (delivery_state != NULL)
					{
						amqpvalue_destroy(delivery_state);
					}
				}

				transfer_destroy(transfer_handle);
			}
		}
	}
	else if (is_disposition_type_by_descriptor(descriptor))
	{
		DISPOSITION_HANDLE disposition;
		if (amqpvalue_get_disposition(performative, &disposition) != 0)
		{
			/* error */
		}
		else
		{
			delivery_number first;
			delivery_number last;

			if (disposition_get_first(disposition, &first) != 0)
			{
				/* error */
			}
			else
			{
				if (disposition_get_last(disposition, &last) != 0)
				{
					last = first;
				}

				LIST_ITEM_HANDLE pending_delivery = list_get_head_item(link_instance->pending_deliveries);
				while (pending_delivery != NULL)
				{
					LIST_ITEM_HANDLE next_pending_delivery = list_get_next_item(pending_delivery);
					DELIVERY_INSTANCE* delivery_instance = (DELIVERY_INSTANCE*)list_item_get_value(pending_delivery);
					if (delivery_instance == NULL)
					{
						/* error */
						break;
					}
					else
					{
						delivery_instance->on_delivery_settled(delivery_instance->callback_context, delivery_instance->delivery_id);
						amqpalloc_free(delivery_instance);
						if (list_remove(link_instance->pending_deliveries, pending_delivery) != 0)
						{
							/* error */
							break;
						}
						else
						{
							pending_delivery = next_pending_delivery;
						}
					}
				}
			}

			disposition_destroy(disposition);
		}
	}
	else if (is_detach_type_by_descriptor(descriptor))
	{
		if (send_detach_frame(link_instance, NULL) != 0)
		{
			/* error */
		}
	}
}

static int send_attach(LINK_INSTANCE* link, const char* name, handle handle, role role)
{
	int result;
	ATTACH_HANDLE attach = attach_create(name, handle, role);

	if (attach == NULL)
	{
		result = __LINE__;
	}
	else
	{
		result = 0;

		link->delivery_count = link->initial_delivery_count;

		attach_set_snd_settle_mode(attach, link->snd_settle_mode);
		attach_set_rcv_settle_mode(attach, link->rcv_settle_mode);
		attach_set_role(attach, role);
		attach_set_source(attach, link->source);
		attach_set_target(attach, link->target);
        attach_set_properties(attach, link->attach_properties);

		if (role == role_sender)
		{
			if (attach_set_initial_delivery_count(attach, link->delivery_count) != 0)
			{
				result = __LINE__;
			}
		}

		if (result == 0)
		{
			if ((attach_set_max_message_size(attach, link->max_message_size) != 0) ||
				(session_send_attach(link->link_endpoint, attach) != 0))
			{
				result = __LINE__;
			}
			else
			{
				result = 0;
			}
		}

		attach_destroy(attach);
	}

	return result;
}

static int send_detach(LINK_INSTANCE* link_instance, ERROR_HANDLE error)
{
	int result;

	DETACH_HANDLE detach = detach_create(0);
	if (detach == NULL)
	{
		result = __LINE__;
	}
	else
	{
		if (((error != NULL) && (detach_set_error(detach, error) != 0)) ||
			 (session_send_detach(link_instance->link_endpoint, detach) != 0))
		{
			result = __LINE__;
		}
		else
		{
			result = 0;
		}

		detach_destroy(detach);
	}

	return result;
}

static void on_session_state_changed(void* context, SESSION_STATE new_session_state, SESSION_STATE previous_session_state)
{
	LINK_INSTANCE* link_instance = (LINK_INSTANCE*)context;

	if (new_session_state == SESSION_STATE_MAPPED)
	{
		if (link_instance->link_state == LINK_STATE_DETACHED)
		{
			if (send_attach(link_instance, link_instance->name, 0, link_instance->role) == 0)
			{
				set_link_state(link_instance, LINK_STATE_HALF_ATTACHED);
			}
		}
	}
	else if (new_session_state == SESSION_STATE_DISCARDING)
	{
		set_link_state(link_instance, LINK_STATE_DETACHED);
	}
}

static void on_session_flow_on(void* context)
{
	LINK_INSTANCE* link_instance = (LINK_INSTANCE*)context;
	if (link_instance->role == role_sender)
	{
		link_instance->on_link_flow_on(link_instance->callback_context);
	}
}

static void on_send_complete(void* context, IO_SEND_RESULT send_result)
{
	LIST_ITEM_HANDLE delivery_instance_list_item = (LIST_ITEM_HANDLE)context;
	DELIVERY_INSTANCE* delivery_instance = (DELIVERY_INSTANCE*)list_item_get_value(delivery_instance_list_item);
	LINK_INSTANCE* link_instance = (LINK_INSTANCE*)delivery_instance->link;
	if (link_instance->snd_settle_mode == sender_settle_mode_settled)
	{
		delivery_instance->on_delivery_settled(delivery_instance->callback_context, delivery_instance->delivery_id);
		amqpalloc_free(delivery_instance);
		(void)list_remove(link_instance->pending_deliveries, delivery_instance_list_item);
	}
}

LINK_HANDLE link_create(SESSION_HANDLE session, const char* name, role role, AMQP_VALUE source, AMQP_VALUE target)
{
	LINK_INSTANCE* result = amqpalloc_malloc(sizeof(LINK_INSTANCE));
	if (result != NULL)
	{
		result->link_state = LINK_STATE_DETACHED;
		result->previous_link_state = LINK_STATE_DETACHED;
		result->role = role;
		result->source = amqpvalue_clone(source);
		result->target = amqpvalue_clone(target);
		result->session = session;
		result->handle = 0;
		result->snd_settle_mode = sender_settle_mode_unsettled;
		result->rcv_settle_mode = receiver_settle_mode_first;
		result->delivery_count = 0;
		result->initial_delivery_count = 0;
		result->max_message_size = 0;
		result->is_underlying_session_begun = 0;
        result->attach_properties = NULL;

		result->pending_deliveries = list_create();
		if (result->pending_deliveries == NULL)
		{
			amqpalloc_free(result);
			result = NULL;
		}
		else
		{
			result->name = amqpalloc_malloc(strlen(name) + 1);
			if (result->name == NULL)
			{
				list_destroy(result->pending_deliveries);
				amqpalloc_free(result);
				result = NULL;
			}
			else
			{
				result->on_link_state_changed = NULL;
				result->callback_context = NULL;
				set_link_state(result, LINK_STATE_DETACHED);

				(void)strcpy(result->name, name);
				result->link_endpoint = session_create_link_endpoint(session, name);
				if (result->link_endpoint == NULL)
				{
					list_destroy(result->pending_deliveries);
					amqpalloc_free(result->name);
					amqpalloc_free(result);
					result = NULL;
				}
			}
		}
	}

	return result;
}

LINK_HANDLE link_create_from_endpoint(SESSION_HANDLE session, LINK_ENDPOINT_HANDLE link_endpoint, const char* name, role role, AMQP_VALUE source, AMQP_VALUE target)
{
	LINK_INSTANCE* result = amqpalloc_malloc(sizeof(LINK_INSTANCE));
	if (result != NULL)
	{
		result->link_state = LINK_STATE_DETACHED;
		result->previous_link_state = LINK_STATE_DETACHED;
		result->session = session;
		result->handle = 0;
		result->snd_settle_mode = sender_settle_mode_unsettled;
		result->rcv_settle_mode = receiver_settle_mode_first;
		result->delivery_count = 0;
		result->initial_delivery_count = 0;
		result->max_message_size = 0;
		result->is_underlying_session_begun = 0;
		result->source = amqpvalue_clone(target);
		result->target = amqpvalue_clone(source);
		if (role == role_sender)
		{
			result->role = role_receiver;
		}
		else
		{
			result->role = role_sender;
		}

		result->pending_deliveries = list_create();
		if (result->pending_deliveries == NULL)
		{
			amqpalloc_free(result);
			result = NULL;
		}
		else
		{
			result->name = amqpalloc_malloc(strlen(name) + 1);
			if (result->name == NULL)
			{
				list_destroy(result->pending_deliveries);
				amqpalloc_free(result);
				result = NULL;
			}
			else
			{
				(void)strcpy(result->name, name);
				result->on_link_state_changed = NULL;
				result->callback_context = NULL;
				set_link_state(result, LINK_STATE_HALF_ATTACHED);
				result->link_endpoint = link_endpoint;
			}
		}
	}

	return result;
}

void link_destroy(LINK_HANDLE link)
{
	if (link != NULL)
	{
		link_detach(link);

		session_destroy_link_endpoint(link->link_endpoint);
		amqpvalue_destroy(link->source);
		amqpvalue_destroy(link->target);
		if (link->pending_deliveries != NULL)
		{
			LIST_ITEM_HANDLE item = list_get_head_item(link->pending_deliveries);
			while (item != NULL)
			{
				LIST_ITEM_HANDLE next_item = list_get_next_item(item);
				DELIVERY_INSTANCE* delivery_instance = (DELIVERY_INSTANCE*)list_item_get_value(item);
				if (delivery_instance != NULL)
				{
					amqpalloc_free(delivery_instance);
				}

				item = next_item;
			}

			list_destroy(link->pending_deliveries);
		}

		if (link->name != NULL)
		{
			amqpalloc_free(link->name);
		}

        if (link->attach_properties != NULL)
        {
            amqpvalue_destroy(link->attach_properties);
        }

		amqpalloc_free(link);
	}
}

int link_set_snd_settle_mode(LINK_HANDLE link, sender_settle_mode snd_settle_mode)
{
	int result;

	if (link == NULL)
	{
		result = __LINE__;
	}
	else
	{
		link->snd_settle_mode = snd_settle_mode;
		result = 0;
	}

	return result;
}

int link_get_snd_settle_mode(LINK_HANDLE link, sender_settle_mode* snd_settle_mode)
{
	int result;

	if ((link == NULL) ||
		(snd_settle_mode == NULL))
	{
		result = __LINE__;
	}
	else
	{
		*snd_settle_mode = link->snd_settle_mode;

		result = 0;
	}

	return result;
}

int link_set_rcv_settle_mode(LINK_HANDLE link, receiver_settle_mode rcv_settle_mode)
{
	int result;

	if (link == NULL)
	{
		result = __LINE__;
	}
	else
	{
		link->rcv_settle_mode = rcv_settle_mode;
		result = 0;
	}

	return result;
}

int link_get_rcv_settle_mode(LINK_HANDLE link, receiver_settle_mode* rcv_settle_mode)
{
	int result;

	if ((link == NULL) ||
		(rcv_settle_mode == NULL))
	{
		result = __LINE__;
	}
	else
	{
		*rcv_settle_mode = link->rcv_settle_mode;
		result = 0;
	}

	return result;
}

int link_set_initial_delivery_count(LINK_HANDLE link, sequence_no initial_delivery_count)
{
	int result;

	if (link == NULL)
	{
		result = __LINE__;
	}
	else
	{
		link->initial_delivery_count = initial_delivery_count;
		result = 0;
	}

	return result;
}

int link_get_initial_delivery_count(LINK_HANDLE link, sequence_no* initial_delivery_count)
{
	int result;

	if ((link == NULL) ||
		(initial_delivery_count == NULL))
	{
		result = __LINE__;
	}
	else
	{
		*initial_delivery_count = link->initial_delivery_count;
		result = 0;
	}

	return result;
}

int link_set_max_message_size(LINK_HANDLE link, uint64_t max_message_size)
{
	int result;

	if (link == NULL)
	{
		result = __LINE__;
	}
	else
	{
		link->max_message_size = max_message_size;
		result = 0;
	}

	return result;
}

int link_get_max_message_size(LINK_HANDLE link, uint64_t* max_message_size)
{
	int result;

	if ((link == NULL) ||
		(max_message_size == NULL))
	{
		result = __LINE__;
	}
	else
	{
		*max_message_size = link->max_message_size;
		result = 0;
	}

	return result;
}

int link_set_attach_properties(LINK_HANDLE link, fields attach_properties)
{
    int result;

    if (link == NULL)
    {
        result = __LINE__;
    }
    else
    {
        link->attach_properties = amqpvalue_clone(attach_properties);
        if (link->attach_properties == NULL)
        {
            result = __LINE__;
        }
        else
        {
            result = 0;
        }
    }

    return result;
}

int link_attach(LINK_HANDLE link, ON_TRANSFER_RECEIVED on_transfer_received, ON_LINK_STATE_CHANGED on_link_state_changed, ON_LINK_FLOW_ON on_link_flow_on, void* callback_context)
{
	int result;

	if (link == NULL)
	{
		result = __LINE__;
	}
	else
	{
		if (!link->is_underlying_session_begun)
		{
			link->on_link_state_changed = on_link_state_changed;
			link->on_transfer_received = on_transfer_received;
			link->on_link_flow_on = on_link_flow_on;
			link->callback_context = callback_context;

			if (session_begin(link->session) != 0)
			{
				result = __LINE__;
			}
			else
			{
				link->is_underlying_session_begun = 1;

				if (session_start_link_endpoint(link->link_endpoint, link_frame_received, on_session_state_changed, on_session_flow_on, link) != 0)
				{
					result = __LINE__;
				}
				else
				{
					result = 0;
				}
			}
		}
		else
		{
			result = 0;
		}
	}

	return result;
}

int link_detach(LINK_HANDLE link)
{
	int result;

	if (link == NULL)
	{
		result = __LINE__;
	}
	else
	{
		if ((link->link_state == LINK_STATE_HALF_ATTACHED) ||
			(link->link_state == LINK_STATE_ATTACHED))
		{
			link->on_link_state_changed = NULL;

			if (send_detach(link, NULL) != 0)
			{
				result = __LINE__;
			}
			else
			{
				set_link_state(link, LINK_STATE_DETACHED);
				result = 0;
			}
		}
		else
		{
			set_link_state(link, LINK_STATE_DETACHED);
			result = 0;
		}
	}

	return result;
}

LINK_TRANSFER_RESULT link_transfer(LINK_HANDLE link, PAYLOAD* payloads, size_t payload_count, ON_DELIVERY_SETTLED on_delivery_settled, void* callback_context)
{
	LINK_TRANSFER_RESULT result;

	if (link == NULL)
	{
		result = LINK_TRANSFER_ERROR;
	}
	else
	{
		if (link->role != role_sender)
		{
			result = LINK_TRANSFER_ERROR;
		}
		else if (link->link_credit == 0)
		{
			result = LINK_TRANSFER_BUSY;
		}
		else
		{
			TRANSFER_HANDLE transfer = transfer_create(0);
			if (transfer == NULL)
			{
				result = LINK_TRANSFER_ERROR;
			}
			else
			{
				unsigned char delivery_tag_bytes[sizeof(link->delivery_count)];
				delivery_tag delivery_tag;
				bool settled;

				(void)memcpy(delivery_tag_bytes, &link->delivery_count, sizeof(link->delivery_count));

				link->delivery_count++;

				delivery_tag.bytes = &delivery_tag_bytes;
				delivery_tag.length = sizeof(delivery_tag_bytes);

				if (link->snd_settle_mode == sender_settle_mode_unsettled)
				{
					settled = false;
				}
				else
				{
					settled = true;
				}

				if ((transfer_set_delivery_tag(transfer, delivery_tag) != 0) ||
					(transfer_set_message_format(transfer, 0) != 0) ||
					(transfer_set_settled(transfer, settled) != 0))
				{
					result = LINK_TRANSFER_ERROR;
				}
				else
				{
					AMQP_VALUE transfer_value = amqpvalue_create_transfer(transfer);

					if (transfer_value == NULL)
					{
						result = LINK_TRANSFER_ERROR;
					}
					else
					{
						DELIVERY_INSTANCE* pending_delivery = amqpalloc_malloc(sizeof(DELIVERY_INSTANCE));
						if (pending_delivery == NULL)
						{
							result = LINK_TRANSFER_ERROR;
						}
						else
						{
							LIST_ITEM_HANDLE delivery_instance_list_item;
							pending_delivery->on_delivery_settled = on_delivery_settled;
							pending_delivery->callback_context = callback_context;
							pending_delivery->link = link;
							delivery_instance_list_item = list_add(link->pending_deliveries, pending_delivery);

							if (delivery_instance_list_item == NULL)
							{
								amqpalloc_free(pending_delivery);
								result = LINK_TRANSFER_ERROR;
							}
							else
							{
								/* here we should feed data to the transfer frame */
								switch (session_send_transfer(link->link_endpoint, transfer, payloads, payload_count, &pending_delivery->delivery_id, on_send_complete, delivery_instance_list_item))
								{
								default:
								case SESSION_SEND_TRANSFER_ERROR:
									result = LINK_TRANSFER_ERROR;
									break;

								case SESSION_SEND_TRANSFER_BUSY:
									result = LINK_TRANSFER_BUSY;
									break;

								case SESSION_SEND_TRANSFER_OK:
									link->link_credit--;
									result = LINK_TRANSFER_OK;
									break;
								}
							}
						}

						amqpvalue_destroy(transfer_value);
					}
				}

				transfer_destroy(transfer);
			}
		}
	}

	return result;
}
