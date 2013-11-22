/*
 *
 *  oFono - Open Source Telephony - RIL Modem Support
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2013 Canonical, Ltd. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sim.h>

#include "ofono.h"

#include "simutil.h"
#include "util.h"

#include "gril.h"
#include "grilutil.h"
#include "parcel.h"
#include "ril_constants.h"
#include "rilmodem.h"

#include "grilreply.h"
#include "grilrequest.h"
#include "grilunsol.h"

/*
 * Based on ../drivers/atmodem/sim.c.
 *
 * TODO:
 * 1. Defines constants for hex literals
 * 2. Document P1-P3 usage (+CSRM)
 */

/*
 * TODO: CDMA/IMS
 *
 * This code currently only grabs the AID/application ID from
 * the gsm_umts application on the SIM card.  This code will
 * need to be modified for CDMA support, and possibly IMS-based
 * applications.  In this case, app_id should be changed to an
 * array or HashTable of app_status structures.
 *
 * The same applies to the app_type.
 */
struct sim_data {
	GRil *ril;
	gchar *aid_str;
	guint app_type;
	gchar *app_str;
	guint app_index;
	gboolean sim_registered;
	enum ofono_sim_password_type passwd_type;
	int retries[OFONO_SIM_PASSWORD_INVALID];
	enum ofono_sim_password_type passwd_state;
};

static void ril_file_info_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_file_info_cb_t cb = cbd->cb;
	struct sim_data *sd = cbd->user;
	struct ofono_error error;
	gboolean ok = FALSE;
	int sw1, sw2;
	int flen = 0, rlen = 0, str = 0;
	guchar access[3] = { 0x00, 0x00, 0x00 };
	guchar file_status = EF_STATUS_VALID;
	struct reply_sim_io *reply;

	if (message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");
	} else {
		ofono_error("Reply failure: %s",
				ril_error_to_string(message->error));
		decode_ril_error(&error, "FAIL");
		goto error;
	}

	if ((reply = g_ril_reply_parse_sim_io(sd->ril, message))
			== NULL) {
		ofono_error("Can't parse SIM IO response from RILD");
		decode_ril_error(&error, "FAIL");
		goto error;
	}

	sw1 = reply->sw1;
	sw2 = reply->sw2;
	if ((sw1 != 0x90 && sw1 != 0x91 && sw1 != 0x92 && sw1 != 0x9f) ||
		(sw1 == 0x90 && sw2 != 0x00)) {
		ofono_error("Error reply, invalid values: sw1: %02x sw2: %02x",
				sw1, sw2);
		memset(&error, 0, sizeof(error));

		/* TODO: fix decode_ril_error to take type & error */

		error.type = OFONO_ERROR_TYPE_SIM;
		error.error = (sw1 << 8) | sw2;

		goto err_aft_reply;
	}

	if (reply->hex_len) {
		if (reply->hex_response[0] == 0x62) {
			ok = sim_parse_3g_get_response(reply->hex_response,
							reply->hex_len,
							&flen, &rlen, &str,
							access, NULL);
		} else {
			ok = sim_parse_2g_get_response(reply->hex_response,
							reply->hex_len,
							&flen, &rlen, &str,
							access, &file_status);
		}
	}

	if (!ok) {
		ofono_error("parse response failed");
		decode_ril_error(&error, "FAIL");
		goto err_aft_reply;
	}

	cb(&error, flen, str, rlen, access, file_status, cbd->data);

	g_ril_reply_free_sim_io(reply);

	return;

err_aft_reply:
	g_ril_reply_free_sim_io(reply);
error:
	cb(&error, -1, -1, -1, NULL, EF_STATUS_INVALIDATED, cbd->data);
}

static void ril_sim_read_info(struct ofono_sim *sim, int fileid,
				const unsigned char *path, unsigned int path_len,
				ofono_sim_file_info_cb_t cb,
				void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data, sd);
	struct parcel rilp;
	struct req_sim_read_info req;
	int request = RIL_REQUEST_SIM_IO;
	guint ret = 0;

	req.app_type = sd->app_type;
	req.aid_str = sd->aid_str;
	req.fileid = fileid;
	req.path = path;
	req.path_len = path_len;

	if (!g_ril_request_sim_read_info(sd->ril,
					&req,
					&rilp)) {
		ofono_error("Couldn't build SIM read info request");
		goto error;
	}

	ret = g_ril_send(sd->ril,
				request,
				rilp.data,
				rilp.size,
				ril_file_info_cb, cbd, g_free);

	g_ril_append_print_buf(sd->ril,
				"%s0,0,15,(null),pin2=(null),aid=%s)",
				print_buf,
				sd->aid_str);
	g_ril_print_request(sd->ril, ret, request);

	parcel_free(&rilp);

error:
	if (ret == 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, -1, -1, -1, NULL,
				EF_STATUS_INVALIDATED, data);
	}
}

static void ril_file_io_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_read_cb_t cb = cbd->cb;
	struct sim_data *sd = cbd->user;
	struct ofono_error error;
	struct reply_sim_io *reply;

	if (message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");
	} else {
		ofono_error("RILD reply failure: %s",
				ril_error_to_string(message->error));
		goto error;
	}

	if ((reply = g_ril_reply_parse_sim_io(sd->ril, message))
			== NULL) {
		ofono_error("Can't parse SIM IO response from RILD");
		decode_ril_error(&error, "FAIL");
		goto error;
	}

	cb(&error, reply->hex_response, reply->hex_len, cbd->data);

	g_ril_reply_free_sim_io(reply);

	return;

error:
	decode_ril_error(&error, "FAIL");
	cb(&error, NULL, 0, cbd->data);
}

static void ril_sim_read_binary(struct ofono_sim *sim, int fileid,
				int start, int length,
				const unsigned char *path, unsigned int path_len,
				ofono_sim_read_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data, sd);
	struct parcel rilp;
	struct req_sim_read_binary req;
	int request = RIL_REQUEST_SIM_IO;
	guint ret = 0;

	req.app_type = sd->app_type;
	req.aid_str = sd->aid_str;
	req.fileid = fileid;
	req.path = path;
	req.path_len = path_len;
	req.start = start;
	req.length = length;

	if (!g_ril_request_sim_read_binary(sd->ril,
						&req,
						&rilp)) {
		ofono_error("Couldn't build SIM read binary request");
		goto error;
	}

	ret = g_ril_send(sd->ril,
				request,
				rilp.data,
				rilp.size,
				ril_file_io_cb, cbd, g_free);

	g_ril_append_print_buf(sd->ril,
				"%s%d,%d,%d,(null),pin2=(null),aid=%s)",
				print_buf,
				(start >> 8),
				(start & 0xff),
				length,
				sd->aid_str);
	g_ril_print_request(sd->ril, ret, request);

	parcel_free(&rilp);

error:
	if (ret == 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
	}
}

static void ril_sim_read_record(struct ofono_sim *sim, int fileid,
				int record, int length,
				const unsigned char *path, unsigned int path_len,
				ofono_sim_read_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data, sd);
	struct parcel rilp;
	struct req_sim_read_record req;
	int request = RIL_REQUEST_SIM_IO;
	guint ret = 0;

	req.app_type = sd->app_type;
	req.aid_str = sd->aid_str;
	req.fileid = fileid;
	req.path = path;
	req.path_len = path_len;
	req.record = record;
	req.length = length;

	if (!g_ril_request_sim_read_record(sd->ril,
						&req,
						&rilp)) {
		ofono_error("Couldn't build SIM read record request");
		goto error;
	}

	ret = g_ril_send(sd->ril,
				request,
				rilp.data,
				rilp.size,
				ril_file_io_cb, cbd, g_free);

	g_ril_append_print_buf(sd->ril,
				"%s%d,%d,%d,(null),pin2=(null),aid=%s)",
				print_buf,
				record,
				4,
				length,
				sd->aid_str);
	g_ril_print_request(sd->ril, ret, request);

	parcel_free(&rilp);

error:
	if (ret == 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
	}
}

static void ril_imsi_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_imsi_cb_t cb = cbd->cb;
	struct sim_data *sd = cbd->user;
	struct ofono_error error;
	gchar *imsi;

	if (message->error == RIL_E_SUCCESS) {
		DBG("GET IMSI reply - OK");
		decode_ril_error(&error, "OK");
	} else {
		ofono_error("Reply failure: %s",
				ril_error_to_string(message->error));
		goto error;
	}

	imsi = g_ril_reply_parse_imsi(sd->ril, message);
	if (imsi == NULL) {
		ofono_error("Error empty IMSI");
		goto error;
	}

	cb(&error, imsi, cbd->data);
	g_free(imsi);

	return;

error:
	decode_ril_error(&error, "FAIL");
	cb(&error, NULL, cbd->data);
}

static void ril_read_imsi(struct ofono_sim *sim, ofono_sim_imsi_cb_t cb,
				void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data, sd);
	struct parcel rilp;
	int request = RIL_REQUEST_GET_IMSI;
	guint ret;

	g_ril_request_read_imsi(sd->ril,
				sd->aid_str,
				&rilp);

	ret = g_ril_send(sd->ril, request,
				rilp.data, rilp.size, ril_imsi_cb, cbd, g_free);

	g_ril_print_request(sd->ril, ret, request);

	parcel_free(&rilp);

	if (ret == 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, NULL, data);
	}
}

static void configure_active_app(struct sim_data *sd,
					struct reply_sim_app *app,
					guint index)
{
	g_free(sd->aid_str);
	g_free(sd->app_str);
	sd->app_type = app->app_type;
	sd->aid_str = g_strdup(app->aid_str);
	sd->app_str = g_strdup(app->app_str);
	sd->app_index = index;

	DBG("setting aid_str (AID) to: %s", sd->aid_str);
	switch (app->app_state) {
	case APPSTATE_PIN:
		sd->passwd_state = OFONO_SIM_PASSWORD_SIM_PIN;
		break;
	case APPSTATE_PUK:
		sd->passwd_state = OFONO_SIM_PASSWORD_SIM_PUK;
		break;
	case APPSTATE_SUBSCRIPTION_PERSO:
		switch (app->perso_substate) {
		case PERSOSUBSTATE_SIM_NETWORK:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHNET_PIN;
			break;
		case PERSOSUBSTATE_SIM_NETWORK_SUBSET:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHNETSUB_PIN;
			break;
		case PERSOSUBSTATE_SIM_CORPORATE:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHCORP_PIN;
			break;
		case PERSOSUBSTATE_SIM_SERVICE_PROVIDER:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHSP_PIN;
			break;
		case PERSOSUBSTATE_SIM_SIM:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHSIM_PIN;
			break;
		case PERSOSUBSTATE_SIM_NETWORK_PUK:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHNET_PUK;
			break;
		case PERSOSUBSTATE_SIM_NETWORK_SUBSET_PUK:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHNETSUB_PUK;
			break;
		case PERSOSUBSTATE_SIM_CORPORATE_PUK:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHCORP_PUK;
			break;
		case PERSOSUBSTATE_SIM_SERVICE_PROVIDER_PUK:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHSP_PUK;
			break;
		case PERSOSUBSTATE_SIM_SIM_PUK:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHFSIM_PUK;
			break;
		default:
			sd->passwd_state = OFONO_SIM_PASSWORD_NONE;
			break;
		};
		break;
	case APPSTATE_READY:
		sd->passwd_state = OFONO_SIM_PASSWORD_NONE;
		break;
	case APPSTATE_UNKNOWN:
	case APPSTATE_DETECTED:
	default:
		sd->passwd_state = OFONO_SIM_PASSWORD_INVALID;
		break;
	}
}

static void sim_status_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_sim *sim = user_data;
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct reply_sim_status *status;
	guint i = 0;
	guint search_index = -1;

	if ((status = g_ril_reply_parse_sim_status(sd->ril, message))
			!= NULL
			&& status->card_state == RIL_CARDSTATE_PRESENT
			&& status->num_apps) {

		DBG("num_apps: %d gsm_umts_index: %d", status->num_apps,
			status->gsm_umts_index);

		/*
		 * TODO(CDMA): need some kind of logic to
		 * set the correct app_index,
		 */
		search_index = status->gsm_umts_index;

		for (i = 0; i < status->num_apps; i++) {
			struct reply_sim_app *app = status->apps[i];
			if (i == search_index &&
					app->app_type != RIL_APPTYPE_UNKNOWN) {
				configure_active_app(sd, app, i);
				break;
			}
		}

		if (sd->sim_registered == FALSE) {
			/* First status request, after sim_probe() */
			ofono_sim_register(sim);
			sd->sim_registered = TRUE;
		} else {
			/* status request afer entering PIN */
			/*
			 * TODO: There doesn't seem to be any other
			 * way to force the core SIM code to
			 * recheck the PIN.
			 * Wouldn't __ofono_sim_refresh be
			 * more appropriate call here??
			 * __ofono_sim_refresh(sim, NULL, TRUE, TRUE);
			 */
			__ofono_sim_recheck_pin(sim);
		}

		g_ril_reply_free_sim_status(status);
	}

	/* TODO: if no SIM present, handle emergency calling. */
}


static int send_get_sim_status(struct ofono_sim *sim)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	int request = RIL_REQUEST_GET_SIM_STATUS;
	guint ret;

	ret = g_ril_send(sd->ril, request,
				NULL, 0, sim_status_cb, sim, NULL);

	g_ril_print_request_no_args(sd->ril, ret, request);

	return ret;
}

static void ril_sim_status_changed(struct ril_msg *message, gpointer user_data)
{
	struct ofono_sim *sim = (struct ofono_sim *) user_data;
	struct sim_data *sd = ofono_sim_get_data(sim);

	DBG("");

	g_ril_print_unsol_no_args(sd->ril, message);

	send_get_sim_status(sim);
}

static void ril_query_pin_retries(struct ofono_sim *sim,
					ofono_sim_pin_retries_cb_t cb,
					void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	CALLBACK_WITH_SUCCESS(cb, sd->retries, data);
}

static void ril_query_passwd_state(struct ofono_sim *sim,
					ofono_sim_passwd_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	DBG("passwd_state %u", sd->passwd_state);

	if (sd->passwd_state == OFONO_SIM_PASSWORD_INVALID)
		CALLBACK_WITH_FAILURE(cb, -1, data);
	else
		CALLBACK_WITH_SUCCESS(cb, sd->passwd_state, data);
}

static void ril_pin_change_state_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_lock_unlock_cb_t cb = cbd->cb;
	struct ofono_sim *sim = cbd->data;
	struct sim_data *sd = cbd->user;
	/*
	 * There is no reason to ask SIM status until
	 * unsolicited sim status change indication
	 * Looks like state does not change before that.
	 */

	DBG("Enter password: type %d, result %d",
		sd->passwd_type, message->error);

	/* TODO: re-bfactor to not use macro for FAILURE;
	   doesn't return error! */
	if (message->error == RIL_E_SUCCESS) {
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
		g_ril_print_response_no_args(sd->ril, message);

	} else {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		/*
		 * Refresh passwd_state (not needed if the unlock is
		 * successful, as an event will refresh the state in that case)
		 */
		send_get_sim_status(sim);
	}
}

static void ril_pin_send(struct ofono_sim *sim, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data, sd);
	struct parcel rilp;
	int request = RIL_REQUEST_ENTER_SIM_PIN;
	int ret;

	sd->passwd_type = OFONO_SIM_PASSWORD_SIM_PIN;

	g_ril_request_pin_send(sd->ril,
				passwd,
				sd->aid_str,
				&rilp);

	ret = g_ril_send(sd->ril, request,
				rilp.data, rilp.size, ril_pin_change_state_cb,
				cbd, g_free);

	g_ril_print_request(sd->ril, ret, request);

	parcel_free(&rilp);

	if (ret == 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static void ril_pin_change_state(struct ofono_sim *sim,
					enum ofono_sim_password_type passwd_type,
					int enable, const char *passwd,
					ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data, sd);
	struct parcel rilp;
	struct req_pin_change_state req;
	int request = RIL_REQUEST_SET_FACILITY_LOCK;
	int ret = 0;

	sd->passwd_type = passwd_type;

	req.aid_str = sd->aid_str;
	req.passwd_type = passwd_type;
	req.enable = enable;
	req.passwd = passwd;

	if (!g_ril_request_pin_change_state(sd->ril,
						&req,
						&rilp)) {
		ofono_error("Couldn't build pin change state request");
		goto error;
	}

	ret = g_ril_send(sd->ril, request,
				rilp.data, rilp.size, ril_pin_change_state_cb,
				cbd, g_free);

	g_ril_print_request(sd->ril, ret, request);

	parcel_free(&rilp);

error:
	if (ret == 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static void ril_pin_send_puk(struct ofono_sim *sim,
				const char *puk, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data, sd);
	struct parcel rilp;
	int request = RIL_REQUEST_ENTER_SIM_PUK;
	int ret = 0;

	sd->passwd_type = OFONO_SIM_PASSWORD_SIM_PUK;

	g_ril_request_pin_send_puk(sd->ril,
					puk,
					passwd,
					sd->aid_str,
					&rilp);

	ret = g_ril_send(sd->ril, request,
			rilp.data, rilp.size, ril_pin_change_state_cb,
			cbd, g_free);

	g_ril_print_request(sd->ril, ret, request);

	parcel_free(&rilp);

	if (ret == 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static void ril_change_passwd(struct ofono_sim *sim,
				enum ofono_sim_password_type passwd_type,
				const char *old_passwd, const char *new_passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data, sd);
	struct parcel rilp;
	int request = RIL_REQUEST_CHANGE_SIM_PIN;
	int ret = 0;

	sd->passwd_type = passwd_type;

	g_ril_request_change_passwd(sd->ril,
					old_passwd,
					new_passwd,
					sd->aid_str,
					&rilp);

	if (passwd_type == OFONO_SIM_PASSWORD_SIM_PIN2)
		request = RIL_REQUEST_CHANGE_SIM_PIN2;

	ret = g_ril_send(sd->ril, request, rilp.data, rilp.size,
			ril_pin_change_state_cb, cbd, g_free);

	g_ril_print_request(sd->ril, ret, request);

	parcel_free(&rilp);

	if (ret == 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static gboolean ril_sim_register(gpointer user)
{
	struct ofono_sim *sim = user;
	struct sim_data *sd = ofono_sim_get_data(sim);

 	DBG("");

 	send_get_sim_status(sim);

	g_ril_register(sd->ril, RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
			(GRilNotifyFunc) ril_sim_status_changed, sim);

	/* TODO: should we also register for RIL_UNSOL_SIM_REFRESH? */

	return FALSE;
}

static int ril_sim_probe(struct ofono_sim *sim, unsigned int vendor,
				void *data)
{
	GRil *ril = data;
	struct sim_data *sd;
	int i;

	sd = g_new0(struct sim_data, 1);
	sd->ril = g_ril_clone(ril);
	sd->aid_str = NULL;
	sd->app_str = NULL;
	sd->app_type = RIL_APPTYPE_UNKNOWN;
	sd->passwd_state = OFONO_SIM_PASSWORD_NONE;
	sd->passwd_type = OFONO_SIM_PASSWORD_NONE;
	sd->sim_registered = FALSE;

	/*
	 * The number of retries is unreliable in the current RIL
	 * implementation of Google devices (Galaxy Nexus and Nexus 4 return
	 * always 0 and 1 respectively in ENTER_SIM_PIN/PUK), so we never
	 * refresh this value after calling those RIL requests.
	 */
	for (i = 0; i < OFONO_SIM_PASSWORD_INVALID; i++)
		sd->retries[i] = -1;

	ofono_sim_set_data(sim, sd);

	/*
	 * TODO: analyze if capability check is needed
	 * and/or timer should be adjusted.
	 *
	 * ofono_sim_register() needs to be called after the
	 * driver has been set in ofono_sim_create(), which
	 * calls this function.  Most other drivers make some
	 * kind of capabilities query to the modem, and then
	 * call register in the callback; we use an idle event
	 * instead.
	 */
	g_idle_add(ril_sim_register, sim);

	return 0;
}

static void ril_sim_remove(struct ofono_sim *sim)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	ofono_sim_set_data(sim, NULL);

	g_ril_unref(sd->ril);
	g_free(sd->aid_str);
	g_free(sd->app_str);
	g_free(sd);
}

static struct ofono_sim_driver driver = {
	.name			= RILMODEM,
	.probe			= ril_sim_probe,
	.remove			= ril_sim_remove,
	.read_file_info		= ril_sim_read_info,
	.read_file_transparent	= ril_sim_read_binary,
	.read_file_linear	= ril_sim_read_record,
	.read_file_cyclic	= ril_sim_read_record,
 	.read_imsi		= ril_read_imsi,
	.query_passwd_state	= ril_query_passwd_state,
	.send_passwd		= ril_pin_send,
	.lock			= ril_pin_change_state,
	.reset_passwd		= ril_pin_send_puk,
	.change_passwd		= ril_change_passwd,
	.query_pin_retries	= ril_query_pin_retries,
/*
 * TODO: Implmenting PIN/PUK support requires defining
 * the following driver methods.
 *
 * In the meanwhile, as long as the SIM card is present,
 * and unlocked, the core SIM code will check for the
 * presence of query_passwd_state, and if null, then the
 * function sim_initialize_after_pin() is called.
 *
 *	.query_pin_retries	= ril_pin_retries_query,
 *	.query_locked		= ril_pin_query_enabled,
 *
 * TODO: Implementing SIM write file IO support requires
 * the following functions to be defined.
 *
 *	.write_file_transparent	= ril_sim_update_binary,
 *	.write_file_linear	= ril_sim_update_record,
 *	.write_file_cyclic	= ril_sim_update_cyclic,
 */
};

void ril_sim_init(void)
{
	DBG("");
	ofono_sim_driver_register(&driver);
}

void ril_sim_exit(void)
{
	ofono_sim_driver_unregister(&driver);
}
