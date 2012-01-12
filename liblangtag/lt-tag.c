/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* 
 * lt-tag.c
 * Copyright (C) 2011-2012 Akira TAGOH
 * 
 * Authors:
 *   Akira TAGOH  <akira@tagoh.org>
 * 
 * This library is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include "lt-database.h"
#include "lt-error.h"
#include "lt-mem.h"
#include "lt-tag.h"


typedef enum _lt_tag_state_t {
	STATE_NONE,
	STATE_LANG,
	STATE_PRE_EXTLANG,
	STATE_EXTLANG,
	STATE_PRE_SCRIPT,
	STATE_SCRIPT,
	STATE_PRE_REGION,
	STATE_REGION,
	STATE_PRE_VARIANT,
	STATE_VARIANT,
	STATE_PRE_EXTENSION,
	STATE_EXTENSION,
	STATE_IN_EXTENSION,
	STATE_EXTENSIONTOKEN,
	STATE_PRE_PRIVATEUSE,
	STATE_PRIVATEUSE,
	STATE_IN_PRIVATEUSE,
	STATE_PRIVATEUSETOKEN
} lt_tag_state_t;
typedef struct _lt_tag_scanner_t {
	lt_mem_t  parent;
	gchar    *string;
	gsize     length;
	gsize     position;
} lt_tag_scanner_t;

struct _lt_tag_t {
	lt_mem_t            parent;
	gchar              *tag_string;
	lt_lang_t          *language;
	lt_extlang_t       *extlang;
	lt_script_t        *script;
	lt_region_t        *region;
	lt_variant_t       *variant;
	GString            *extension;
	GString            *privateuse;
	lt_grandfathered_t *grandfathered;
};

/*< private >*/
static void
_lt_tag_gstring_free(GString *string)
{
	g_string_free(string, TRUE);
}

static lt_tag_scanner_t *
lt_tag_scanner_new(const gchar *tag)
{
	lt_tag_scanner_t *retval = lt_mem_alloc_object(sizeof (lt_tag_scanner_t));

	if (retval) {
		retval->string = g_strdup(tag);
		lt_mem_add_ref(&retval->parent, retval->string,
			       (lt_destroy_func_t)g_free);
		retval->length = strlen(tag);
	}

	return retval;
}

static void
lt_tag_scanner_unref(lt_tag_scanner_t *scanner)
{
	if (scanner)
		lt_mem_unref(&scanner->parent);
}

static gboolean
lt_tag_scanner_get_token(lt_tag_scanner_t  *scanner,
			 gchar            **retval,
			 gsize             *length,
			 GError           **error)
{
	GString *string = NULL;
	gchar c;
	GError *err = NULL;

	g_return_val_if_fail (scanner != NULL, FALSE);

	if (scanner->position >= scanner->length) {
		g_set_error(&err, LT_ERROR, LT_ERR_EOT,
			    "No more tokens in buffer");
		goto bail;
	}

	string = g_string_new(NULL);
	while (scanner->position < scanner->length) {
		c = scanner->string[scanner->position++];
		if (c == 0) {
			if (string->len == 0) {
				g_set_error(&err, LT_ERROR, LT_ERR_EOT,
					    "No more tokens in buffer");
			}
			scanner->position--;
			break;
		}
		if (!g_ascii_isalnum(c) && c != '-' && c != 0) {
			g_set_error(&err, LT_ERROR, LT_ERR_FAIL_ON_SCANNER,
				    "Invalid character for tag: '%c'", c);
			break;
		}
		g_string_append_c(string, c);

		if (c == '-')
			break;
		if (scanner->string[scanner->position] == '-' ||
		    scanner->string[scanner->position] == 0)
			break;
	}
  bail:
	if (err) {
		if (error)
			*error = g_error_copy(err);
		else
			g_warning(err->message);
		g_error_free(err);
		if (string)
			g_string_free(string, TRUE);
		*retval = NULL;
		*length = 0;

		return FALSE;
	}

	*length = string->len;
	*retval = g_string_free(string, FALSE);

	return TRUE;
}

static gboolean
lt_tag_scanner_is_eof(lt_tag_scanner_t *scanner)
{
	g_return_val_if_fail (scanner != NULL, TRUE);
	g_return_val_if_fail (scanner->position <= scanner->length, TRUE);

	return scanner->string[scanner->position] == 0 ||
		scanner->position >= scanner->length;
}

static gboolean
lt_tag_parse_prestate(lt_tag_t        *tag,
		      const gchar     *token,
		      gsize            length,
		      lt_tag_state_t  *state,
		      GError         **error)
{
	gboolean retval = TRUE;

	if (g_strcmp0(token, "-") == 0) {
		switch (*state) {
		    case STATE_PRE_EXTLANG:
			    *state = STATE_EXTLANG;
			    break;
		    case STATE_PRE_SCRIPT:
			    *state = STATE_SCRIPT;
			    break;
		    case STATE_PRE_REGION:
			    *state = STATE_REGION;
			    break;
		    case STATE_PRE_VARIANT:
			    *state = STATE_VARIANT;
			    break;
		    case STATE_PRE_EXTENSION:
			    *state = STATE_EXTENSION;
			    break;
		    case STATE_IN_EXTENSION:
			    *state = STATE_EXTENSIONTOKEN;
			    break;
		    case STATE_PRE_PRIVATEUSE:
			    *state = STATE_PRIVATEUSE;
			    break;
		    case STATE_IN_PRIVATEUSE:
			    *state = STATE_PRIVATEUSETOKEN;
			    break;
		    default:
			    g_set_error(error, LT_ERROR, LT_ERR_FAIL_ON_SCANNER,
					"Invalid syntax found during parsing a token: %s",
					token);
			    retval = FALSE;
			    break;
		}
	} else {
		retval = FALSE;
	}

	return retval;
}

static gboolean
lt_tag_parse_state(lt_tag_t        *tag,
		   const gchar     *token,
		   gsize            length,
		   lt_tag_state_t  *state,
		   GError         **error)
{
	gboolean retval = TRUE;
	const gchar *p;

	switch (*state) {
	    case STATE_LANG:
		    if (length == 1) {
			    if (g_ascii_strcasecmp(token, "x") == 0) {
				    *state = STATE_IN_PRIVATEUSE;
				    break;
			    } else {
			      invalid_tag:
				    g_set_error(error, LT_ERROR, LT_ERR_FAIL_ON_SCANNER,
						"Invalid tag: %s", tag->tag_string);
				    break;
			    }
		    } else if (length >= 2 && length <= 3) {
			    lt_lang_db_t *langdb = lt_db_get_lang();

			    /* shortest ISO 639 code */
			    tag->language = lt_lang_db_lookup(langdb, token);
			    lt_lang_db_unref(langdb);
			    if (!tag->language) {
				    g_set_error(error, LT_ERROR, LT_ERR_FAIL_ON_SCANNER,
						"Unknown ISO 639 code: %s",
						token);
				    break;
			    }
			    /* validate if it's really shortest one */
			    p = lt_lang_get_tag(tag->language);
			    if (!p || g_ascii_strcasecmp(token, p) != 0) {
				    g_set_error(error, LT_ERROR, LT_ERR_FAIL_ON_SCANNER,
						"No such language subtag: %s",
						token);
				    lt_lang_unref(tag->language);
				    tag->language = NULL;
				    break;
			    }
			    lt_mem_add_ref(&tag->parent, tag->language,
					   (lt_destroy_func_t)lt_lang_unref);
			    *state = STATE_PRE_EXTLANG;
		    } else if (length == 4) {
			    /* reserved for future use */
			    g_set_error(error, LT_ERROR, LT_ERR_FAIL_ON_SCANNER,
					"Reserved for future use: %s",
					token);
		    } else if (length >= 5 && length <= 8) {
			    /* registered language subtag */
			    g_set_error(error, LT_ERROR, LT_ERR_FAIL_ON_SCANNER,
					"XXX: registered language tag: %s",
					token);
		    } else {
			    goto invalid_tag;
		    }
		    break;
	    case STATE_EXTLANG:
		    if (length == 3) {
			    lt_extlang_db_t *extlangdb = lt_db_get_extlang();

			    tag->extlang = lt_extlang_db_lookup(extlangdb, token);
			    lt_extlang_db_unref(extlangdb);
			    if (tag->extlang) {
				    const gchar *macrolang = lt_extlang_get_macro_language(tag->extlang);
				    const gchar *subtag = lt_extlang_get_tag(tag->extlang);
				    const gchar *lang = lt_lang_get_better_tag(tag->language);

				    if (macrolang &&
					g_ascii_strcasecmp(macrolang, lang) != 0) {
					    g_set_error(error, LT_ERROR, LT_ERR_FAIL_ON_SCANNER,
							"extlang '%s' is supposed to be used with %s, but %s",
							subtag, macrolang, lang);
					    lt_extlang_unref(tag->extlang);
					    tag->extlang = NULL;
				    } else {
					    lt_mem_add_ref(&tag->parent, tag->extlang,
							   (lt_destroy_func_t)lt_extlang_unref);
					    *state = STATE_PRE_SCRIPT;
				    }
				    break;
			    }
			    /* try to check something else */
		    } else {
			    /* it may be a script */
			    *state = STATE_SCRIPT;
		    }
	    case STATE_SCRIPT:
		    if (length == 4) {
			    lt_script_db_t *scriptdb = lt_db_get_script();

			    tag->script = lt_script_db_lookup(scriptdb, token);
			    lt_script_db_unref(scriptdb);
			    if (tag->script) {
				    lt_mem_add_ref(&tag->parent, tag->script,
						   (lt_destroy_func_t)lt_script_unref);
				    *state = STATE_PRE_REGION;
				    break;
			    }
			    /* try to check something else */
		    } else {
			    /* it may be a region */
			    *state = STATE_REGION;
		    }
	    case STATE_REGION:
		    if (length == 2 ||
			(length == 3 &&
			 g_ascii_isdigit(token[0]) &&
			 g_ascii_isdigit(token[1]) &&
			 g_ascii_isdigit(token[2]))) {
			    lt_region_db_t *regiondb = lt_db_get_region();

			    g_print("'%s'\n", token);
			    tag->region = lt_region_db_lookup(regiondb, token);
			    lt_region_db_unref(regiondb);
			    if (tag->region) {
				    g_print("found\n");
				    lt_mem_add_ref(&tag->parent, tag->region,
						   (lt_destroy_func_t)lt_region_unref);
				    *state = STATE_PRE_VARIANT;
				    break;
			    }
			    /* try to check something else */
		    } else {
			    /* it may be a variant */
			    *state = STATE_VARIANT;
		    }
	    case STATE_VARIANT:
		    if ((length >=5 && length <= 8) ||
			(length == 4 && g_ascii_isdigit(token[0]))) {
			    lt_variant_db_t *variantdb = lt_db_get_variant();

			    tag->variant = lt_variant_db_lookup(variantdb, token);
			    lt_variant_db_unref(variantdb);
			    if (tag->variant) {
				    const GList *prefixes = lt_variant_get_prefix(tag->variant), *l;
				    const gchar *lang = lt_lang_get_better_tag(tag->language);
				    GString *str_prefixes = g_string_new(NULL);
				    gboolean matched = FALSE;

				    for (l = prefixes; l != NULL; l = g_list_next(l)) {
					    if (str_prefixes->len > 0)
						    g_string_append(str_prefixes, ",");
					    g_string_append(str_prefixes, (const gchar *)l->data);

					    if (g_ascii_strcasecmp((gchar *)l->data,
								   lang) == 0) {
						    matched = TRUE;
						    break;
					    }
				    }
				    if (!matched) {
					    g_set_error(error, LT_ERROR, LT_ERR_FAIL_ON_SCANNER,
							"variant '%s' is supposed to be used with %s, but %s",
							token, str_prefixes->str, lang);
					    lt_variant_unref(tag->variant);
					    tag->variant = NULL;
				    } else {
					    lt_mem_add_ref(&tag->parent, tag->variant,
							   (lt_destroy_func_t)lt_variant_unref);
					    *state = STATE_PRE_EXTENSION;
				    }
				    g_string_free(str_prefixes, TRUE);
				    break;
			    }
			    /* try to check something else */
		    } else {
			    /* it may be an extension */
			    *state = STATE_EXTENSION;
		    }
	    case STATE_EXTENSION:
		    if (length == 1 && token[0] != 'x' && token[0] != 'X') {
			    g_string_append(tag->extension, token);
			    *state = STATE_IN_EXTENSION;
			    break;
		    } else {
			    /* it may be a private use */
			    *state = STATE_PRIVATEUSE;
		    }
	    case STATE_PRIVATEUSE:
		    if (length == 1 && (token[0] == 'x' || token[0] == 'X')) {
			    g_string_append(tag->privateuse, token);
			    *state = STATE_IN_PRIVATEUSE;
		    } else {
			    /* No state to try */
			    retval = FALSE;
		    }
		    break;
	    case STATE_EXTENSIONTOKEN:
		    if (length >= 2 && length <= 8) {
			    g_string_append_printf(tag->extension, "-%s", token);
			    *state = STATE_PRIVATEUSE;
		    } else {
			    /* No state to try */
			    retval = FALSE;
		    }
		    break;
	    case STATE_PRIVATEUSETOKEN:
		    if (length <= 8) {
			    g_string_append_printf(tag->privateuse, "-%s", token);
			    *state = STATE_NONE;
		    } else {
			    /* 'x'/'X' is reserved singleton for the private use subtag.
			     * so nothing to fallback to anything else.
			     */
			    g_set_error(error, LT_ERROR, LT_ERR_FAIL_ON_SCANNER,
					"Invalid tag for the private use: token = '%s'",
					token);
		    }
		    break;
	    default:
		    g_set_error(error, LT_ERROR, LT_ERR_FAIL_ON_SCANNER,
				"Unable to parse tag: %s, token = '%s' state = %d",
				tag->tag_string, token, *state);
		    break;
	}
	if (*error)
		retval = FALSE;

	return retval;
}

/*< public >*/
lt_tag_t *
lt_tag_new(void)
{
	lt_tag_t *retval = lt_mem_alloc_object(sizeof (lt_tag_t));

	if (retval) {
		retval->extension = g_string_new(NULL);
		lt_mem_add_ref(&retval->parent, retval->extension,
			       (lt_destroy_func_t)_lt_tag_gstring_free);
		retval->privateuse = g_string_new(NULL);
		lt_mem_add_ref(&retval->parent, retval->privateuse,
			       (lt_destroy_func_t)_lt_tag_gstring_free);
	}

	return retval;
}

lt_tag_t *
lt_tag_ref(lt_tag_t *tag)
{
	g_return_val_if_fail (tag != NULL, NULL);

	return lt_mem_ref(&tag->parent);
}

void
lt_tag_unref(lt_tag_t *tag)
{
	if (tag)
		lt_mem_unref(&tag->parent);
}

gboolean
lt_tag_parse(lt_tag_t     *tag,
	     const gchar  *langtag,
	     GError      **error)
{
	lt_tag_scanner_t *scanner;
	lt_grandfathered_db_t *grandfathereddb;
	gchar *token = NULL;
	gsize len = 0;
	GError *err = NULL;
	gboolean retval = TRUE;
	lt_tag_state_t state = STATE_LANG;
	gint count = 0;

	g_return_val_if_fail (tag != NULL, FALSE);
	g_return_val_if_fail (langtag != NULL, FALSE);

	if (tag->tag_string)
		lt_mem_remove_ref(&tag->parent, tag->tag_string);
	if (tag->language) {
		lt_mem_remove_ref(&tag->parent, tag->language);
		tag->language = NULL;
	}
	if (tag->extlang) {
		lt_mem_remove_ref(&tag->parent, tag->extlang);
		tag->extlang = NULL;
	}
	if (tag->script) {
		lt_mem_remove_ref(&tag->parent, tag->script);
		tag->script = NULL;
	}
	if (tag->region) {
		lt_mem_remove_ref(&tag->parent, tag->region);
		tag->region = NULL;
	}
	if (tag->variant) {
		lt_mem_remove_ref(&tag->parent, tag->variant);
		tag->variant = NULL;
	}
	if (tag->extension) {
		g_string_truncate(tag->extension, 0);
	}
	if (tag->privateuse) {
		g_string_truncate(tag->privateuse, 0);
	}
	if (tag->grandfathered) {
		lt_mem_remove_ref(&tag->parent, tag->grandfathered);
		tag->grandfathered = NULL;
	}
	tag->tag_string = g_strdup(langtag);

	grandfathereddb = lt_db_get_grandfathered();
	tag->grandfathered = lt_grandfathered_db_lookup(grandfathereddb, langtag);
	lt_grandfathered_db_unref(grandfathereddb);
	if (tag->grandfathered) {
		/* no need to lookup anymore. */
		lt_mem_add_ref(&tag->parent, tag->grandfathered,
			       (lt_destroy_func_t)lt_grandfathered_unref);

		return TRUE;
	}

	lt_mem_add_ref(&tag->parent, tag->tag_string,
		       (lt_destroy_func_t)g_free);
	scanner = lt_tag_scanner_new(langtag);
	while (!lt_tag_scanner_is_eof(scanner)) {
		if (token) {
			g_free(token);
			token = NULL;
		}
		if (!lt_tag_scanner_get_token(scanner, &token, &len, &err)) {
			if (err)
				break;
			g_set_error(&err, LT_ERROR, LT_ERR_FAIL_ON_SCANNER,
				    "Unrecoverable error");
			break;
		}
		count++;
		if (!token || len == 0) {
			g_set_error(&err, LT_ERROR, LT_ERR_FAIL_ON_SCANNER,
				    "No valid tokens found");
			break;
		}
		if (!lt_tag_parse_prestate(tag, token, len, &state, &err)) {
			if (err)
				break;
			if (!lt_tag_parse_state(tag, token, len, &state, &err))
				break;
		}
	}
	if (state != STATE_PRE_EXTLANG &&
	    state != STATE_PRE_SCRIPT &&
	    state != STATE_PRE_REGION &&
	    state != STATE_PRE_VARIANT &&
	    state != STATE_PRE_EXTENSION &&
	    state != STATE_PRE_PRIVATEUSE &&
	    state != STATE_NONE) {
		g_set_error(&err, LT_ERROR, LT_ERR_FAIL_ON_SCANNER,
			    "Invalid tag: %s, last token = '%s', state = %d, parsed count = %d",
			    langtag, token, state, count);
	}
	lt_tag_scanner_unref(scanner);
	if (err) {
		if (error)
			*error = g_error_copy(err);
		else
			g_warning(err->message);
		g_error_free(err);
		retval = FALSE;
	}
	g_free(token);

	return retval;
}

gchar *
lt_tag_convert_to_locale(lt_tag_t  *tag,
			 GError   **error)
{
	gchar *retval = NULL;
	GString *string = NULL;
	GError *err = NULL;
	const gchar *mod = NULL;

	g_return_val_if_fail (tag != NULL, NULL);

	if (!tag->tag_string) {
		g_set_error(&err, LT_ERROR, LT_ERR_NO_TAG,
			    "No tag to convert.");
		goto bail;
	}
	string = g_string_new(NULL);
	g_string_append(string, lt_lang_get_better_tag(tag->language));
	if (tag->region)
		g_string_append_printf(string, "_%s",
				       lt_region_get_tag(tag->region));
	if (tag->script) {
		mod = lt_script_convert_to_modifier(tag->script);
		if (mod)
			g_string_append_printf(string, "@%s", mod);
	}

  bail:
	if (string)
		retval = g_string_free(string, FALSE);
	if (err) {
		if (error)
			*error = g_error_copy(err);
		else
			g_warning(err->message);
		g_error_free(err);
		if (retval)
			g_free(retval);
		retval = NULL;
	}

	return retval;
}

void
lt_tag_dump(lt_tag_t *tag)
{
	g_return_if_fail (tag != NULL);

	if (tag->grandfathered) {
		g_print("Grandfathered: %s (%s)\n",
			lt_grandfathered_get_tag(tag->grandfathered),
			lt_grandfathered_get_name(tag->grandfathered));
		return;
	}
	g_print("Language: %s (%s)\n",
		lt_lang_get_better_tag(tag->language),
		lt_lang_get_name(tag->language));
	if (tag->extlang)
		g_print("Extlang: %s (%s)\n",
			lt_extlang_get_tag(tag->extlang),
			lt_extlang_get_name(tag->extlang));
	if (tag->script)
		g_print("Script: %s (%s)\n",
			lt_script_get_tag(tag->script),
			lt_script_get_name(tag->script));
	if (tag->region)
		g_print("Region: %s (%s)\n",
			lt_region_get_tag(tag->region),
			lt_region_get_name(tag->region));
	if (tag->variant)
		g_print("Variant: %s (%s)\n",
			lt_variant_get_tag(tag->variant),
			lt_variant_get_name(tag->variant));
	if (tag->extension->len > 0)
		g_print("Extension: %s\n", tag->extension->str);
	if (tag->privateuse->len > 0)
		g_print("Private Use: %s\n", tag->privateuse->str);
}