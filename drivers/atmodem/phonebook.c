/*
 * oFono - GSM Telephony Stack for Linux
 *
 * Copyright (C) 2008-2009 Intel Corporation.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <glib.h>

#include <ofono/log.h>
#include "driver.h"
#include "util.h"

#include "gatchat.h"
#include "gatresult.h"

#include "at.h"

#define INDEX_INVALID -1

#define CHARSET_UTF8 1
#define CHARSET_UCS2 2
#define CHARSET_SUPPORT (CHARSET_UTF8 | CHARSET_UCS2)

static const char *none_prefix[] = { NULL };
static const char *cpbr_prefix[] = { "+CPBR:", NULL };
static const char *cscs_prefix[] = { "+CSCS:", NULL };
static const char *cpbs_prefix[] = { "+CPBS:", NULL };

struct pb_data {
	int index_min, index_max;
	char *old_charset;
	int supported;
};

static struct pb_data *phonebook_create()
{
	struct pb_data *pb = g_try_new0(struct pb_data, 1);
	return pb;
}

static void phonebook_destroy(struct pb_data *data)
{
	if (data->old_charset)
		g_free(data->old_charset);
	g_free(data);
}

static char *ucs2_to_utf8(const char *str)
{
	long len;
	unsigned char *ucs2;
	char *utf8;
	ucs2 = decode_hex(str, -1, &len, 0);
	utf8 = g_convert((char *)ucs2, len, "UTF-8//TRANSLIT", "UCS-2BE",
					NULL, NULL, NULL);
	g_free(ucs2);
	return utf8;
}

static const char *best_charset(int supported)
{
	const char *charset = "Invalid";

	if (supported & CHARSET_UCS2)
		charset = "UCS2";

	if (supported & CHARSET_UTF8)
		charset = "UTF-8";

	return charset;
}

static void at_cpbr_notify(GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_modem *modem = cbd->modem;
	struct at_data *at = ofono_modem_userdata(modem);
	GAtResultIter iter;
	int current;

	dump_response("at_cbpr_notify", 1, result);

	if (at->pb->supported & CHARSET_UCS2)
		current = CHARSET_UCS2;

	if (at->pb->supported & CHARSET_UTF8)
		current = CHARSET_UTF8;

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+CPBR:")) {
		int index;
		const char *number;
		int type;
		const char *text;
		int hidden = -1;
		const char *group = NULL;
		const char *adnumber = NULL;
		int adtype = -1;
		const char *secondtext = NULL;
		const char *email = NULL;
		const char *sip_uri = NULL;
		const char *tel_uri = NULL;

		if (!g_at_result_iter_next_number(&iter, &index))
			continue;

		if (!g_at_result_iter_next_string(&iter, &number))
			continue;

		if (!g_at_result_iter_next_number(&iter, &type))
			continue;

		if (!g_at_result_iter_next_string(&iter, &text))
			continue;

		g_at_result_iter_next_number(&iter, &hidden);
		g_at_result_iter_next_string(&iter, &group);
		g_at_result_iter_next_string(&iter, &adnumber);
		g_at_result_iter_next_number(&iter, &adtype);
		g_at_result_iter_next_string(&iter, &secondtext);
		g_at_result_iter_next_string(&iter, &email);
		g_at_result_iter_next_string(&iter, &sip_uri);
		g_at_result_iter_next_string(&iter, &tel_uri);

		/* charset_current is either CHARSET_UCS2 or CHARSET_UTF8 */
		if (current == CHARSET_UCS2) {
			char *text_utf8;
			char *group_utf8 = NULL;
			char *secondtext_utf8 = NULL;
			char *email_utf8 = NULL;
			char *sip_uri_utf8 = NULL;
			char *tel_uri_utf8 = NULL;

			text_utf8 = ucs2_to_utf8(text);
			if (group)
				group_utf8 = ucs2_to_utf8(group);
			if (secondtext)
				secondtext_utf8 = ucs2_to_utf8(secondtext);
			if (email)
				email_utf8 = ucs2_to_utf8(email);
			if (sip_uri)
				sip_uri_utf8 = ucs2_to_utf8(sip_uri);
			if (tel_uri)
				tel_uri_utf8 = ucs2_to_utf8(tel_uri);

			ofono_phonebook_entry(cbd->modem, index, number, type,
				text_utf8, hidden, group_utf8, adnumber,
				adtype, secondtext_utf8, email_utf8,
				sip_uri_utf8, tel_uri_utf8);

			g_free(text_utf8);
			g_free(group_utf8);
			g_free(secondtext_utf8);
			g_free(email_utf8);
			g_free(sip_uri_utf8);
			g_free(tel_uri_utf8);
		} else {
			ofono_phonebook_entry(cbd->modem, index, number, type,
				text, hidden, group, adnumber,
				adtype, secondtext, email,
				sip_uri, tel_uri);

		}
	}
}

static void export_failed(struct cb_data *cbd)
{
	struct ofono_modem *modem = cbd->modem;
	struct at_data *at = ofono_modem_userdata(modem);
	ofono_generic_cb_t cb = cbd->cb;

	{
		DECLARE_FAILURE(error);
		cb(&error, cbd->data);
	}

	g_free(cbd);

	if (at->pb->old_charset) {
		g_free(at->pb->old_charset);
		at->pb->old_charset = NULL;
	}
}

static void at_read_entries_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_modem *modem = cbd->modem;
	struct at_data *at = ofono_modem_userdata(modem);
	ofono_generic_cb_t cb = cbd->cb;
	const char *charset;
	struct ofono_error error;
	char buf[32];

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
	g_free(cbd);

	charset = best_charset(at->pb->supported);

	if (strcmp(at->pb->old_charset, charset)) {
		sprintf(buf, "AT+CSCS=\"%s\"", at->pb->old_charset);
		g_at_chat_send(at->parser, buf, none_prefix, NULL, NULL, NULL);
	}

	g_free(at->pb->old_charset);
	at->pb->old_charset = NULL;
}

static void at_read_entries(struct cb_data *cbd)
{
	struct ofono_modem *modem = cbd->modem;
	struct at_data *at = ofono_modem_userdata(modem);
	char buf[32];

	sprintf(buf, "AT+CPBR=%d,%d", at->pb->index_min, at->pb->index_max);
	if (g_at_chat_send_listing(at->parser, buf, cpbr_prefix,
					at_cpbr_notify, at_read_entries_cb,
					cbd, NULL) > 0)
		return;

	/* If we get here, then most likely connection to the modem dropped
	 * and we can't really restore the charset anyway
	 */
	export_failed(cbd);
}

static void at_set_charset_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;

	if (!ok) {
		export_failed(cbd);
		return;
	}

	at_read_entries(cbd);
}

static void at_read_charset_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_modem *modem = cbd->modem;
	struct at_data *at = ofono_modem_userdata(modem);
	GAtResultIter iter;
	const char *charset;
	char buf[32];

	dump_response("at_read_charset_cb", ok, result);

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSCS:"))
		goto error;

	g_at_result_iter_next_string(&iter, &charset);

	at->pb->old_charset = g_strdup(charset);

	charset = best_charset(at->pb->supported);

	if (!strcmp(at->pb->old_charset, charset)) {
		at_read_entries(cbd);
		return;
	}

	sprintf(buf, "AT+CSCS=\"%s\"", charset);
	if (g_at_chat_send(at->parser, buf, none_prefix,
				at_set_charset_cb, cbd, NULL) > 0)
		return;

error:
	export_failed(cbd);
}

static void at_list_indices_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_modem *modem = cbd->modem;
	struct at_data *at = ofono_modem_userdata(modem);
	GAtResultIter iter;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);
	if (!g_at_result_iter_next(&iter, "+CPBR:"))
		goto error;

	if (!g_at_result_iter_open_list(&iter))
		goto error;

	/* retrieve index_min and index_max from indices
	 * which seems like "(1-150),32,16"
	 */
	if (!g_at_result_iter_next_range(&iter, &at->pb->index_min,
						&at->pb->index_max))
		goto error;

	if (!g_at_result_iter_close_list(&iter))
		goto error;

	if (g_at_chat_send(at->parser, "AT+CSCS?", cscs_prefix,
				at_read_charset_cb, cbd, NULL) > 0)
		return;

error:
	export_failed(cbd);
}

static void at_select_storage_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_modem *modem = cbd->modem;
	struct at_data *at = ofono_modem_userdata(modem);

	dump_response("at_select_storage_cb", ok, result);

	if (!ok)
		goto error;

	if (g_at_chat_send(at->parser, "AT+CPBR=?", cpbr_prefix,
				at_list_indices_cb, cbd, NULL) > 0)
		return;

error:
	export_failed(cbd);
}

static void at_export_entries(struct ofono_modem *modem, const char *storage,
				ofono_generic_cb_t cb, void *data)
{
	struct at_data *at = ofono_modem_userdata(modem);
	struct cb_data *cbd = cb_data_new(modem, cb, data);
	char buf[32];

	if (!cbd)
		goto error;

	sprintf(buf, "AT+CPBS=\"%s\"", storage);
	if (g_at_chat_send(at->parser, buf, none_prefix,
				at_select_storage_cb, cbd, NULL) > 0)
		return;

error:
	if (cbd)
		g_free(cbd);

	{
		DECLARE_FAILURE(error);
		cb(&error, data);
	}
}

static struct ofono_phonebook_ops ops = {
	.export_entries		= at_export_entries
};

static void phonebook_not_supported(struct ofono_modem *modem)
{
	struct at_data *at = ofono_modem_userdata(modem);

	ofono_error("Phonebook not supported by this modem.  If this is in "
			"error please submit patches to support this hardware");
	if (at->pb) {
		phonebook_destroy(at->pb);
		at->pb = NULL;
	}
}

static void at_list_storages_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	gboolean sm_supported = FALSE;
	gboolean me_supported = FALSE;
	gboolean in_list = FALSE;
	GAtResultIter iter;
	const char *storage;

	dump_response("at_list_storages_cb", ok, result);

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);
	if (!g_at_result_iter_next(&iter, "+CPBS:"))
		goto error;

	/* Some modems don't report CPBS in a proper list */
	if (g_at_result_iter_open_list(&iter))
		in_list = TRUE;

	while (g_at_result_iter_next_string(&iter, &storage)) {
		if (!strcmp(storage, "ME"))
			me_supported = TRUE;
		else if (!strcmp(storage, "SM"))
			sm_supported = TRUE;
	}

	if (in_list && !g_at_result_iter_close_list(&iter))
		goto error;

	if (!me_supported && !sm_supported)
		goto error;

	ofono_phonebook_register(modem, &ops);
	return;

error:
	phonebook_not_supported(modem);
}

static void at_list_charsets_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct at_data *at = ofono_modem_userdata(modem);
	gboolean in_list = FALSE;
	GAtResultIter iter;
	const char *charset;

	dump_response("at_list_charsets_cb", ok, result);

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);
	if (!g_at_result_iter_next(&iter, "+CSCS:"))
		goto error;

	/* Some modems don't report CPBS in a proper list */
	if (g_at_result_iter_open_list(&iter))
		in_list = TRUE;

	while (g_at_result_iter_next_string(&iter, &charset)) {
		if (!strcmp(charset, "UTF-8"))
			at->pb->supported |= CHARSET_UTF8;
		else if (!strcmp(charset, "UCS2"))
			at->pb->supported |= CHARSET_UCS2;
	}

	if (in_list && g_at_result_iter_close_list(&iter))
		goto error;

	if (!(at->pb->supported & CHARSET_SUPPORT))
		goto error;

	if (g_at_chat_send(at->parser, "AT+CPBS=?", cpbs_prefix,
				at_list_storages_cb, modem, NULL) > 0)
		return;

error:
	phonebook_not_supported(modem);
}

static void at_list_charsets(struct ofono_modem *modem)
{
	struct at_data *at = ofono_modem_userdata(modem);

	if (g_at_chat_send(at->parser, "AT+CSCS=?", cscs_prefix,
				at_list_charsets_cb, modem, NULL) > 0)
		return;

	phonebook_not_supported(modem);
}

void at_phonebook_init(struct ofono_modem *modem)
{
	struct at_data *at = ofono_modem_userdata(modem);

	at->pb = phonebook_create();
	at_list_charsets(modem);
}

void at_phonebook_exit(struct ofono_modem *modem)
{
	struct at_data *at = ofono_modem_userdata(modem);

	if (!at->pb)
		return;

	phonebook_destroy(at->pb);
	at->pb = NULL;

	ofono_phonebook_unregister(modem);
}
