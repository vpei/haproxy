#include <haproxy/stats-file.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <import/ebmbtree.h>
#include <import/ebsttree.h>
#include <import/ist.h>
#include <haproxy/api.h>
#include <haproxy/buf.h>
#include <haproxy/chunk.h>
#include <haproxy/errors.h>
#include <haproxy/global.h>
#include <haproxy/guid-t.h>
#include <haproxy/list.h>
#include <haproxy/listener-t.h>
#include <haproxy/obj_type.h>
#include <haproxy/proxy-t.h>
#include <haproxy/server-t.h>
#include <haproxy/stats.h>

/* Dump all fields from <stats> into <out> for stats-file. */
int stats_dump_fields_file(struct buffer *out,
                           const struct field *line, size_t stats_count,
                           struct show_stat_ctx *ctx)
{
	struct guid_node *guid;
	struct listener *l;
	int i;

	switch (ctx->px_st) {
	case STAT_PX_ST_FE:
	case STAT_PX_ST_BE:
		guid = &__objt_proxy(ctx->obj1)->guid;
		break;

	case STAT_PX_ST_LI:
		l = LIST_ELEM(ctx->obj2, struct listener *, by_fe);
		guid = &l->guid;
		break;

	case STAT_PX_ST_SV:
		guid = &__objt_server(ctx->obj2)->guid;
		break;

	default:
		ABORT_NOW();
		return 1;
	}

	/* Skip objects without GUID. */
	if (!guid->node.key)
		return 1;

	chunk_appendf(out, "%s,", (char *)guid->node.key);

	for (i = 0; i < stats_count; ++i) {
		/* Empty field for stats-file is used to skip its output,
		 * including any separator.
		 */
		if (field_format(line, i) == FF_EMPTY)
			continue;

		if (!stats_emit_raw_data_field(out, &line[i]))
			return 0;
		if (!chunk_strcat(out, ","))
			return 0;
	}

	chunk_strcat(out, "\n");
	return 1;
}

void stats_dump_file_header(int type, struct buffer *out)
{
	const struct stat_col *col;
	int i;

	/* Caller must specified ither FE or BE. */
	BUG_ON(!(type & ((1 << STATS_TYPE_FE) | (1 << STATS_TYPE_BE))));

	if (type & (1 << STATS_TYPE_FE)) {
		chunk_strcat(out, "#fe guid,");
		for (i = 0; i < ST_I_PX_MAX; ++i) {
			col = &stat_cols_px[i];
			if (stcol_nature(col) == FN_COUNTER && (col->cap & (STATS_PX_CAP_FE|STATS_PX_CAP_LI)))
				chunk_appendf(out, "%s,", col->name);
		}
	}
	else {
		chunk_appendf(out, "#be guid,");
		for (i = 0; i < ST_I_PX_MAX; ++i) {
			col = &stat_cols_px[i];
			if (stcol_nature(col) == FN_COUNTER && (col->cap & (STATS_PX_CAP_BE|STATS_PX_CAP_SRV)))
				chunk_appendf(out, "%s,", col->name);
		}
	}

	chunk_strcat(out, "\n");
}

/* Parse an identified header line <header> starting with '#' character.
 *
 * If the section is recognized, <domain> will point to the current stats-file
 * scope. <cols> will be filled as a matrix to identify each stat_col position
 * using <st_tree> as prefilled proxy stats columns. If stats-file section is
 * unknown, only <domain> will be set to STFILE_DOMAIN_UNSET.
 *
 * Returns 0 on sucess. On fatal error, non-zero is returned and parsing shoud
 * be interrupted.
 */
static int parse_header_line(struct ist header, struct eb_root *st_tree,
                             enum stfile_domain *domain,
                             const struct stat_col *cols[])
{
	enum stfile_domain dom = STFILE_DOMAIN_UNSET;
	struct ist token;
	char last;
	int i;

	header = iststrip(header);
	last = istptr(header)[istlen(header) - 1];
	token = istsplit(&header, ' ');

	/* A header line is considered valid if:
	 * - a space delimiter is found and first token is several chars
	 * - last line character must be a comma separator
	 */
	if (!istlen(header) || istlen(token) == 1 || last != ',')
		goto err;

	if (isteq(token, ist("#fe")))
		dom = STFILE_DOMAIN_PX_FE;
	else if (isteq(token, ist("#be")))
		dom = STFILE_DOMAIN_PX_BE;

	/* Remove 'guid' field. */
	token = istsplit(&header, ',');
	if (!isteq(token, ist("guid"))) {
		/* Fatal error if FE/BE domain without guid token. */
		if (dom == STFILE_DOMAIN_PX_FE || dom == STFILE_DOMAIN_PX_BE)
			goto err;
	}

	/* Unknown domain. Following lines should be ignored until next header. */
	if (dom == STFILE_DOMAIN_UNSET)
		return 0;

	/* Generate matrix of stats column into cols[]. */
	memset(cols, 0, sizeof(void *) * STAT_FILE_MAX_COL_COUNT);

	i = 0;
	while (istlen(header) && i < STAT_FILE_MAX_COL_COUNT) {
		struct stcol_node *col_node;
		const struct stat_col *col;
		struct ebmb_node *node;

		/* Lookup column by its name into <st_tree>. */
		token = istsplit(&header, ',');
		node = ebst_lookup(st_tree, ist0(token));
		if (!node) {
			++i;
			continue;
		}

		col_node = ebmb_entry(node, struct stcol_node, name);
		col = col_node->col;

		/* Ignore column if its cap is not valid with current stats-file section. */
		if ((dom == STFILE_DOMAIN_PX_FE &&
		    !(col->cap & (STATS_PX_CAP_FE|STATS_PX_CAP_LI))) ||
		    (dom == STFILE_DOMAIN_PX_BE &&
		     !(col->cap & (STATS_PX_CAP_BE|STATS_PX_CAP_SRV)))) {
			++i;
			continue;
		}

		cols[i] = col;
		++i;
	}

	*domain = dom;
	return 0;

 err:
	*domain = STFILE_DOMAIN_UNSET;
	return 1;
}

/* Parse a stats-file and preload haproxy internal counters. */
void apply_stats_file(void)
{
	const struct stat_col *cols[STAT_FILE_MAX_COL_COUNT];
	struct eb_root st_tree = EB_ROOT;
	enum stfile_domain domain;
	int valid_format = 0;
	FILE *file;
	struct ist istline;
	char *line = NULL;
	ssize_t len;
	size_t alloc_len;
	int linenum;

	if (!global.stats_file)
		return;

	file = fopen(global.stats_file, "r");
	if (!file) {
		ha_warning("config: Can't load stats file: cannot open file.\n");
		return;
	}

	/* Generate stat columns map indexed by name. */
	if (generate_stat_tree(&st_tree, stat_cols_px)) {
		ha_warning("config: Can't load stats file: not enough memory.\n");
		goto out;
	}

	linenum = 0;
	domain = STFILE_DOMAIN_UNSET;
	while (1) {
		len = getline(&line, &alloc_len, file);
		if (len < 0)
			break;

		++linenum;
		istline = iststrip(ist2(line, len));
		if (!istlen(istline))
			continue;

		if (*istptr(istline) == '#') {
			if (parse_header_line(istline, &st_tree, &domain, cols)) {
				if (!valid_format) {
					ha_warning("config: Invalid stats-file format.\n");
					break;
				}

				ha_warning("config: Ignored stats-file header line '%d'.\n", linenum);
			}

			valid_format = 1;
		}
		else if (domain == STFILE_DOMAIN_UNSET) {
			/* Stop parsing if first line is not a valid header.
			 * Allows to immediately stop reading garbage file.
			 */
			if (!valid_format) {
				ha_warning("config: Invalid stats-file format.\n");
				break;
			}
		}
	}

 out:
	while (!eb_is_empty(&st_tree)) {
		struct ebmb_node *node = ebmb_first(&st_tree);
		struct stcol_node *snode = ebmb_entry(node, struct stcol_node, name);

		ebmb_delete(node);
		ha_free(&snode);
	}

	ha_free(&line);
	fclose(file);
}
