/*
 * GrWebSDR: a web SDR receiver
 *
 * Copyright (C) 2017 Ondřej Lysoněk
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program (see the file COPYING).  If not,
 * see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include "auth.h"
#include "websocket.h"
#include "globals.h"
#include "utils.h"
#include "receiver.h"
#include <atomic>
#include <iostream>
#include <cstring>
#include <string>
#include <sstream>
#include <iomanip>
#include <json-c/json_tokener.h>
#include <cstdio>

using namespace std;

struct json_tokener *tok;

string new_stream_name()
{
	static atomic_int ws_id(0);
	int id = ws_id.fetch_add(1);
	stringstream s;
	s << setbase(36) << setfill('0') << setw(4) << id;
	return s.str();
}

void process_authentication(struct json_object *obj, receiver::sptr rec,
		struct websocket_user_data *data)
{
	struct json_object *tmp, *tmp2;
	string user, pass;

	if (json_object_object_get_ex(obj, "login", &tmp)) {
		data->privileged_changed = true;
		if (!json_object_object_get_ex(tmp, "user", &tmp2))
			return;
		if (json_object_get_type(tmp2) != json_type_string)
			return;
		user = json_object_get_string(tmp2);

		if (!json_object_object_get_ex(tmp, "pass", &tmp2))
			return;
		if (json_object_get_type(tmp2) != json_type_string)
			return;
		pass = json_object_get_string(tmp2);
		if (authenticate(user, pass))
			rec->set_privileged(true);
	} else if (json_object_object_get_ex(obj, "logout", &tmp)) {
		data->privileged_changed = true;
		rec->set_privileged(false);
	}
}

void change_freq_offset(struct json_object *obj, receiver::sptr rec,
		struct websocket_user_data *data)
{
	int offset;
	struct json_object *offset_obj;

	if (!json_object_object_get_ex(obj, "freq_offset", &offset_obj)
			|| json_object_get_type(offset_obj) != json_type_int)
		return;
	offset = json_object_get_int(offset_obj);
	rec->set_freq_offset(offset);
	data->offset_changed = true;
}

void change_hw_freq(struct json_object *obj, receiver::sptr rec)
{
	bool priv;
	struct json_object *freq_obj;
	int freq;

	if (!json_object_object_get_ex(obj, "hw_freq", &freq_obj)
			|| json_object_get_type(freq_obj) != json_type_int)
		return;
	freq = json_object_get_int(freq_obj);

	priv = rec->get_privileged();
	if (!priv || rec->get_source() == nullptr)
		return;
	rec->get_source()->set_center_freq(freq);
	lws_callback_on_writable_all_protocol(ws_context, &protocols[1]);
}

void change_gain(struct json_object *obj, receiver::sptr rec)
{
	struct json_object *gain_obj, *auto_gain_obj;
	double gain;
	bool auto_gain;
	bool gain_set = false;

	if (!rec->get_privileged() || rec->get_source() == nullptr)
		return;

	if (json_object_object_get_ex(obj, "auto_gain", &auto_gain_obj)) {
		if (json_object_get_type(auto_gain_obj) == json_type_boolean) {
			auto_gain = json_object_get_boolean(auto_gain_obj);
			rec->get_source()->set_gain_mode(auto_gain);
			gain_set = true;
		}
	}
	if (json_object_object_get_ex(obj, "gain", &gain_obj)) {
		if (json_object_get_type(gain_obj) == json_type_double) {
			gain = json_object_get_double(gain_obj);
			rec->get_source()->set_gain_mode(false);
			rec->get_source()->set_gain(gain);
			gain_set = true;
		}
	}
	if (gain_set)
		lws_callback_on_writable_all_protocol(ws_context, &protocols[1]);
}

void change_demod(struct json_object *obj, receiver::sptr rec,
		struct websocket_user_data *data)
{
	struct json_object *demod_obj;
	const char *demod;

	if (!json_object_object_get_ex(obj, "demod", &demod_obj)
			|| json_object_get_type(demod_obj) != json_type_string)
		return;
	demod = json_object_get_string(demod_obj);

	topbl->lock();
	rec->change_demod(demod);
	topbl->unlock();
	data->demod_changed = true;
}

void change_source(struct json_object *obj, receiver::sptr rec,
		struct websocket_user_data *data)
{
	struct json_object *source_obj;
	int tmp;
	size_t source_ix;

	if (!json_object_object_get_ex(obj, "source", &source_obj)
			|| json_object_get_type(source_obj) != json_type_int) {
		return;
	}
	tmp = json_object_get_int(source_obj);
	if (tmp < 0)
		return;
	source_ix = (size_t) tmp;

	if (source_ix >= osmosdr_sources.size()) {
		return;
	}
	topbl->lock();
	rec->set_source(source_ix);
	topbl->unlock();
	data->source_changed = true;
	data->offset_changed = true;
}

void attach_current_demod(struct json_object *obj, receiver::sptr rec)
{
	struct json_object *tmp;

	tmp = json_object_new_string(rec->get_current_demod().c_str());
	json_object_object_add(obj, "demod", tmp);
}

void attach_hw_freq(struct json_object *obj, receiver::sptr rec)
{
	struct json_object *val_obj;
	osmosdr::source::sptr src;

	src = rec->get_source();
	if (src == nullptr)
		return;
	val_obj = json_object_new_int(src->get_center_freq());
	json_object_object_add(obj, "hw_freq", val_obj);
}

void attach_gain(struct json_object *obj, receiver::sptr rec)
{
	struct json_object *val_obj;
	osmosdr::source::sptr src;

	src = rec->get_source();
	if (src == nullptr)
		return;
	val_obj = json_object_new_boolean(src->get_gain_mode());
	json_object_object_add(obj, "auto_gain", val_obj);
	val_obj = json_object_new_double(src->get_gain());
	json_object_object_add(obj, "gain", val_obj);
}

void attach_freq_offset(struct json_object *obj, receiver::sptr rec)
{
	struct json_object *val_obj;

	val_obj = json_object_new_int(rec->get_freq_offset());
	json_object_object_add(obj, "freq_offset", val_obj);
}

void attach_source_ix(struct json_object *obj, receiver::sptr rec)
{
	struct json_object *val_obj;
	int ix;

	ix = (int) rec->get_source_ix();
	val_obj = json_object_new_int(ix);
	json_object_object_add(obj, "source_ix", val_obj);
}

void attach_source_description(struct json_object *obj, receiver::sptr rec)
{
	struct json_object *tmp;
	size_t ix;

	ix = rec->get_source_ix();
	tmp = json_object_new_string(sources_info[ix].description.c_str());
	json_object_object_add(obj, "description", tmp);
}

void attach_source_labels(struct json_object *obj)
{
	struct json_object *sources, *tmp;

	sources = json_object_new_array();
	for (source_info_t info : sources_info) {
		tmp = json_object_new_string(info.label.c_str());
		json_object_array_add(sources, tmp);
	}
	json_object_object_add(obj, "sources", sources);
}

void attach_sample_rate(struct json_object *obj, receiver::sptr rec)
{
	struct json_object *tmp;

	tmp = json_object_new_int(rec->get_source()->get_sample_rate());
	json_object_object_add(obj, "sample_rate", tmp);
}

void attach_converter_offset(struct json_object *obj, receiver::sptr rec)
{
	struct json_object *tmp;
	source_info_t i;

	i = sources_info[rec->get_source_ix()];
	tmp = json_object_new_int(i.freq_converter_offset);
	json_object_object_add(obj, "converter_offset", tmp);
}

void attach_source_info(struct json_object *obj, receiver::sptr rec)
{
	struct json_object *tmp;

	if (rec->get_source() == nullptr)
		return;
	tmp = json_object_new_object();
	attach_source_ix(tmp, rec);
	attach_source_description(tmp, rec);
	attach_hw_freq(tmp, rec);
	attach_sample_rate(tmp, rec);
	attach_converter_offset(tmp, rec);
	attach_gain(tmp, rec);
	json_object_object_add(obj, "current_source", tmp);
}

void attach_supported_demods(struct json_object *obj)
{
	struct json_object *demods, *tmp;

	demods = json_object_new_array();
	for (string d : receiver::supported_demods) {
		tmp = json_object_new_string(d.c_str());
		json_object_array_add(demods, tmp);
	}
	json_object_object_add(obj, "supported_demods", demods);
}

void attach_init_data(struct json_object *obj, struct websocket_user_data *data)
{
	struct json_object *tmp;

	tmp = json_object_new_string(data->stream_name);
	json_object_object_add(obj, "stream_name", tmp);

	attach_source_labels(obj);
	attach_supported_demods(obj);
}

void attach_privileged(struct json_object *obj, receiver::sptr rec)
{
	bool val;
	struct json_object *val_obj;

	val = rec->get_privileged();

	val_obj = json_object_new_boolean(val);
	json_object_object_add(obj, "privileged", val_obj);
}

void attach_num_clients(struct json_object *obj)
{
	struct json_object *tmp;

	tmp = json_object_new_int(receiver_map.size());
	json_object_object_add(obj, "num_clients", tmp);
}

int init_websocket()
{
	tok = json_tokener_new();
	if (!tok) {
		cerr << "json_tokener_new() failed." << endl;
		return -1;
	}
	return 0;
}

int create_stream(struct websocket_user_data *data)
{
	int pipe_fds[2];
	string tmp;

	if (pipe(pipe_fds)) {
		perror("pipe");
		return -1;
	}
	if (set_nonblock(pipe_fds[0])) {
		return -1;
	}
	tmp = new_stream_name() + string(".ogg");
	if (tmp.size() > STREAM_NAME_LEN)
		return -1;
	strncpy(data->stream_name, tmp.c_str(), tmp.size());
	data->stream_name[tmp.size()] = '\0';
	receiver_map[data->stream_name] = receiver::make(topbl, pipe_fds);
	// Update number of clients
	lws_callback_on_writable_all_protocol(ws_context, &protocols[1]);
	return 0;
}

int websocket_cb(struct lws *wsi, enum lws_callback_reasons reason,
		void *user, void *in, size_t len)
{
	struct websocket_user_data *data = (struct websocket_user_data *) user;
	(void) wsi;

	switch (reason) {
	case LWS_CALLBACK_PROTOCOL_INIT:
		if (init_websocket())
			return -1;
		break;

	case LWS_CALLBACK_SERVER_WRITEABLE: {
		struct json_object *reply;
		char *buf = data->buf + LWS_PRE;
		receiver::sptr rec;

		auto iter = receiver_map.find(data->stream_name);
		if (iter == receiver_map.end()) {
			return -1;
		}
		rec = iter->second;

		reply = json_object_new_object();
		if (data->initialized && !data->source_changed) {
			attach_hw_freq(reply, rec);
			attach_gain(reply, rec);
		}
		if (!data->initialized) {
			attach_init_data(reply, data);
			data->initialized = true;
		}
		if (data->privileged_changed) {
			attach_privileged(reply, rec);
			data->privileged_changed = false;
		}
		if (data->demod_changed) {
			attach_current_demod(reply, rec);
			data->demod_changed = false;
		}
		if (data->offset_changed) {
			attach_freq_offset(reply, rec);
			data->offset_changed = false;
		}
		if (data->source_changed) {
			attach_source_info(reply, rec);
			data->source_changed = false;
		}
		attach_num_clients(reply);
		strcpy(buf, json_object_get_string(reply));
		json_object_put(reply);
		lws_write(wsi, (unsigned char *) buf, strlen(buf), LWS_WRITE_TEXT);
		break;
	}
	case LWS_CALLBACK_RECEIVE: {
		struct json_object *obj;
		receiver::sptr rec;

		auto iter = receiver_map.find(data->stream_name);
		if (iter == receiver_map.end()) {
			return -1;
		}
		rec = iter->second;

		obj = json_tokener_parse_ex(tok, (char *) in, len);
		if (!obj) {
			cerr << "json parsing failed." << endl;
			break;
		}
		json_tokener_reset(tok);

		change_freq_offset(obj, rec, data);
		change_hw_freq(obj, rec);
		change_gain(obj, rec);
		change_demod(obj, rec, data);
		change_source(obj, rec, data);
		process_authentication(obj, rec, data);
		json_object_put(obj);
		lws_callback_on_writable(wsi);
		break;
	}
	case LWS_CALLBACK_ESTABLISHED: {
		create_stream(data);
		data->initialized = false;
		lws_callback_on_writable(wsi);
		break;
	}
	case LWS_CALLBACK_CLOSED: {
		receiver::sptr rec;
		if (receiver_map.find(data->stream_name) == receiver_map.end()) {
			break;
		}
		rec = receiver_map[data->stream_name];
		if (rec->is_running()) {
			if (count_receivers_running() == 1) {
				topbl->stop();
				topbl->wait();
			}
			topbl->lock();
			rec->stop();
			topbl->unlock();
		}
		receiver_map.erase(data->stream_name);
		// Update number of clients
		lws_callback_on_writable_all_protocol(ws_context, &protocols[1]);
		break;
	}
	default:
		break;
	}
	return 0;
}
