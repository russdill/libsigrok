/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2010 Uwe Hermann <uwe@hermann-uwe.de>
 * Copyright (C) 2013 Bert Vermeulen <bert@biot.com>
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <ctype.h>
#include "config.h" /* Needed for PACKAGE and others. */
#include "libsigrok.h"
#include "libsigrok-internal.h"

/* Message logging helpers with subsystem-specific prefix string. */
#define LOG_PREFIX "output/vcd: "
#define sr_log(l, s, args...) sr_log(l, LOG_PREFIX s, ## args)
#define sr_spew(s, args...) sr_spew(LOG_PREFIX s, ## args)
#define sr_dbg(s, args...) sr_dbg(LOG_PREFIX s, ## args)
#define sr_info(s, args...) sr_info(LOG_PREFIX s, ## args)
#define sr_warn(s, args...) sr_warn(LOG_PREFIX s, ## args)
#define sr_err(s, args...) sr_err(LOG_PREFIX s, ## args)

struct bit_index {
	int bit;
	int sample;
};

struct probe_context {
	GArray *indices;
	char symbol;
	char *name;
	gboolean is_vector;
};

struct context {
	GArray *probeindices;
	GString *header;
	uint8_t *prevsample;
	int period;
	uint64_t samplerate;
	unsigned int unitsize;
	uint64_t samplecount;
};

static char *get_array_str(const char *str, int *idx)
{
	char *ret;
	int end;

	end = strlen(str) - 1;

	/* Ends in '>' */
	if (str[end--] != '>')
		return NULL;

	/* Contains at least one digit before '>' */
	if (end <= 0 || !isdigit(str[end]))
		return NULL;

	while (end > 0 && isdigit(str[end]))
		end--;

	/* Has a '<' and at least one character before it */
	if (!end || str[end] != '<')
		return NULL;

	*idx = strtoul(str + end + 1, NULL, 10);

	ret = g_strdup(str);
	if (!ret)
		return NULL;

	ret[end] = '\0';
	return ret;
}

static struct probe_context *probe_find(GArray *indices, const char *name)
{
	unsigned int i;
	for (i = 0; i < indices->len; i++) {
		struct probe_context *ctx;
		ctx = &g_array_index(indices, struct probe_context, i);
		if (ctx->is_vector && !strcmp(ctx->name, name))
			return ctx;
	}
	return NULL;
}

static gint sort_bits(gconstpointer _a, gconstpointer _b)
{
	const struct bit_index *a = _a;
	const struct bit_index *b = _b;
	return a->bit == b->bit ? 0 : (a->bit < b->bit ? 1 : -1);
}

static const char *vcd_header_comment = "\
$comment\n  Acquisition with %d/%d probes at %s\n$end\n";

static int init(struct sr_output *o)
{
	struct context *ctx;
	struct sr_probe *probe;
	GSList *l;
	GVariant *gvar;
	int num_probes;
	char *samplerate_s, *frequency_s, *timestamp;
	int num_enabled_probes = 0;
	time_t t;
	unsigned int i;

	ctx = g_malloc0(sizeof(struct context));

	o->internal = ctx;
	ctx->probeindices = g_array_new(FALSE, TRUE,
						sizeof(struct probe_context));

	for (l = o->sdi->probes; l; l = l->next) {
		char *array_str;
		struct probe_context *probe_ctx;
		struct bit_index bidx;
		probe = l->data;
		if (!probe->enabled)
			continue;
		bidx.bit = 0;
		bidx.sample = ctx->probeindices->len;
		array_str = get_array_str(probe->name, &bidx.bit);
		if (array_str) {
			probe_ctx = probe_find(ctx->probeindices, array_str);
			if (probe_ctx)
				g_free(array_str);
		} else
			probe_ctx = NULL;
		if (!probe_ctx) {
			ctx->probeindices = g_array_set_size(
					ctx->probeindices,
					ctx->probeindices->len + 1);
			probe_ctx = &g_array_index(ctx->probeindices,
					struct probe_context,
					ctx->probeindices->len - 1);
			probe_ctx->name = array_str ? : probe->name;
			probe_ctx->symbol = '!' + ctx->probeindices->len - 1;
			probe_ctx->indices = g_array_new(FALSE, FALSE,
							sizeof(bidx));
			probe_ctx->is_vector = array_str != NULL;
		}
		probe_ctx->indices = g_array_append_val(probe_ctx->indices,
								bidx);
		num_enabled_probes++;
	}

	if (ctx->probeindices->len > 94) {
		sr_err("VCD only supports 94 probes.");
		return SR_ERR;
	}

	ctx->unitsize = (ctx->probeindices->len + 7) / 8;
	ctx->header = g_string_sized_new(512);
	num_probes = g_slist_length(o->sdi->probes);

	/* timestamp */
	t = time(NULL);
	timestamp = g_strdup(ctime(&t));
	timestamp[strlen(timestamp)-1] = 0;
	g_string_printf(ctx->header, "$date %s $end\n", timestamp);
	g_free(timestamp);

	/* generator */
	g_string_append_printf(ctx->header, "$version %s %s $end\n",
			PACKAGE, PACKAGE_VERSION);

	if (sr_config_get(o->sdi->driver, o->sdi, NULL, SR_CONF_SAMPLERATE,
			&gvar) == SR_OK) {
		ctx->samplerate = g_variant_get_uint64(gvar);
		g_variant_unref(gvar);
		samplerate_s = sr_samplerate_string(ctx->samplerate);
		g_string_append_printf(ctx->header, vcd_header_comment,
				 num_enabled_probes, num_probes, samplerate_s);
		g_free(samplerate_s);
	}

	/* timescale */
	/* VCD can only handle 1/10/100 (s - fs), so scale up first */
	if (ctx->samplerate > SR_MHZ(1))
		ctx->period = SR_GHZ(1);
	else if (ctx->samplerate > SR_KHZ(1))
		ctx->period = SR_MHZ(1);
	else
		ctx->period = SR_KHZ(1);
	frequency_s = sr_period_string(ctx->period);
	g_string_append_printf(ctx->header, "$timescale %s $end\n", frequency_s);
	g_free(frequency_s);

	/* scope */
	g_string_append_printf(ctx->header, "$scope module %s $end\n", PACKAGE);

	/* Wires / channels */
	for (i = 0; i < ctx->probeindices->len; i++) {
		struct probe_context *probe_ctx;
		probe_ctx = &g_array_index(ctx->probeindices,
				struct probe_context, i);
		g_string_append_printf(ctx->header, "$var wire %d %c %s $end\n",
				probe_ctx->indices->len, probe_ctx->symbol,
				probe_ctx->name);
		/* Sort from highest bit to lowest for easy output */
		g_array_sort(probe_ctx->indices, sort_bits);
		if (probe_ctx->is_vector)
			g_free(probe_ctx->name);
	}

	g_string_append(ctx->header, "$upscope $end\n"
			"$enddefinitions $end\n");

	ctx->prevsample = g_malloc0(ctx->unitsize);

	return SR_OK;
}

static int get_bit(uint8_t *bit_array, int idx)
{
	return !!(bit_array[idx / 8] & (((uint8_t) 1) << idx));
}

static int receive(struct sr_output *o, const struct sr_dev_inst *sdi,
		const struct sr_datafeed_packet *packet, GString **out)
{
	const struct sr_datafeed_logic *logic;
	struct context *ctx;
	unsigned int i;
	int curbit, prevbit;
	uint8_t *sample;
	int first;

	(void)sdi;

	*out = NULL;
	if (!o || !o->internal)
		return SR_ERR_ARG;
	ctx = o->internal;

	if (packet->type != SR_DF_LOGIC)
		return SR_OK;

	/* The header is still here, this must be the first packet. */
	first = ctx->header != NULL;

	if (first) {
		*out = ctx->header;
		ctx->header = NULL;
	} else
		*out = g_string_sized_new(512);

	logic = packet->payload;
	for (i = 0; i <= logic->length - logic->unitsize; i += logic->unitsize) {
		unsigned int p;

		ctx->samplecount++;

		sample = logic->data + i;

		if (!first && !memcmp(ctx->prevsample, sample, ctx->unitsize))
			continue;

		g_string_append_printf(*out, "#%" PRIu64 "\n",
				(uint64_t)(((float)ctx->samplecount / ctx->samplerate)
				* ctx->period));

		/*
		 * For first pass, output a dumpvars will the values of all
		 * the variables.
		 */
		if (first)
			g_string_append(*out, "$dumpvars\n");

		for (p = 0; p < ctx->probeindices->len; p++) {
			struct probe_context *probe_ctx;
			unsigned int s;
			int b;
			int last_bit;

			probe_ctx = &g_array_index(ctx->probeindices,
					struct probe_context, p);
			/*
			 * Look for any changed bits for this array (or bit),
			 * VCD only contains deltas/changes of signals.
			 */
			for (s = 0; s < probe_ctx->indices->len; s++) {
				struct bit_index *bidx;
				bidx = &g_array_index(probe_ctx->indices,
						struct bit_index, s);
				curbit = get_bit(sample, bidx->sample);
				prevbit = get_bit(ctx->prevsample, bidx->sample);
				if (prevbit != curbit)
					break;
			}
			if (!first && s != probe_ctx->indices->len)
				/* no changes */
				continue;

			if (probe_ctx->is_vector)
				g_string_append_c(*out, 'b');
			last_bit = 0;
			for (s = 0; s < probe_ctx->indices->len; s++) {
				struct bit_index *bidx;
				bidx = &g_array_index(probe_ctx->indices,
						struct bit_index, s);
				/* Fill in spaces between known bits */
				for (b = last_bit - 1; b > bidx->bit; b--)
					g_string_append_c(*out, 'x');
				curbit = get_bit(sample, bidx->sample);
				g_string_append_printf(*out, "%i", curbit);
			}
			/* Fill in spaces after final known bits */
			for (b = last_bit - 1; b >= 0; b--)
				g_string_append_c(*out, 'x');
			if (probe_ctx->is_vector)
				g_string_append_c(*out, ' ');
			g_string_append_printf(*out, "%c\n", probe_ctx->symbol);
		}

		if (first)
			g_string_append(*out, "$end\n");
		first = FALSE;
		memcpy(ctx->prevsample, sample, ctx->unitsize);
	}

	return SR_OK;
}

static int cleanup(struct sr_output *o)
{
	struct context *ctx;
	unsigned int i;

	if (!o || !o->internal)
		return SR_ERR_ARG;

	ctx = o->internal;

	for (i = 0; i < ctx->probeindices->len; i++) {
		struct probe_context *probe_ctx;
		probe_ctx = &g_array_index(ctx->probeindices,
				struct probe_context, i);
		g_array_free(probe_ctx->indices, TRUE);
	}
	g_array_free(ctx->probeindices, TRUE);
	g_free(ctx);

	return SR_OK;
}

struct sr_output_format output_vcd = {
	.id = "vcd",
	.description = "Value Change Dump (VCD)",
	.df_type = SR_DF_LOGIC,
	.init = init,
	.receive = receive,
	.cleanup = cleanup,
};
