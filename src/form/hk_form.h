/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Form controls without scripts (docs/design/03-engine.md, 9): the kind of each control, the
 * state of a page's controls (values, checkedness, selected options), text editing, and the
 * entry list of a submission encoded as application/x-www-form-urlencoded.
 *
 * Text positions are byte offsets into the UTF-8 value, always on code point boundaries.
 * The module only reads the DOM; control state lives beside it.
 */
#ifndef HK_FORM_H
#define HK_FORM_H

#include "base/hk_vec.h"
#include "dom/hk_dom.h"
#include "net/hk_encoding.h"
#include "net/hk_url.h"

typedef enum hk_control_kind {
    HK_CTRL_NONE = 0,
    HK_CTRL_TEXT, /* single-line: text, search, email, url, tel, number and unknown types */
    HK_CTRL_PASSWORD,
    HK_CTRL_TEXTAREA,
    HK_CTRL_CHECKBOX,
    HK_CTRL_RADIO,
    HK_CTRL_SUBMIT, /* <input type=submit>, <button> (type=submit is the default) */
    HK_CTRL_IMAGE,  /* <input type=image>: a submit button (shown as its alt text) */
    HK_CTRL_RESET,
    HK_CTRL_BUTTON, /* does nothing without scripts */
    HK_CTRL_SELECT,
    HK_CTRL_FILE,   /* not supported: shown disabled */
    HK_CTRL_HIDDEN,
} hk_control_kind;

hk_control_kind hk_control_kind_of(const hk_document *d, hk_node_id el);
/* Text-editable kinds: TEXT, PASSWORD and TEXTAREA. */
bool hk_control_is_text(hk_control_kind k);
/* Disabled by its own attribute or by a disabled <fieldset> ancestor (outside its first <legend>). */
bool hk_control_disabled(const hk_document *d, hk_node_id el);
bool hk_control_readonly(const hk_document *d, hk_node_id el);
/* The label of an <input> button: its value, or the default ("送信", "リセット", the alt text). */
hk_str hk_control_button_label(const hk_document *d, hk_node_id el);
/* The form owner: the form attribute's target, the parser's association, or the nearest ancestor form. */
hk_node_id hk_form_owner(const hk_document *d, hk_node_id el);
/* The control a <label> is for (its for attribute, or its first labelable descendant), or none. */
hk_node_id hk_label_control(const hk_document *d, hk_node_id label);

/* The <option> elements of a <select>, in tree order (including those in <optgroup>). */
typedef HK_VEC(hk_node_id) hk_node_vec;
void hk_select_options(const hk_document *d, hk_node_id select, hk_node_vec *out);
/* An option's label: its label attribute, or its text with whitespace collapsed. */
hk_str hk_option_label(const hk_document *d, hk_node_id option, hk_charbuf *buf);

/* --- state ---------------------------------------------------------------------------------------- */

typedef struct hk_forms hk_forms;

hk_forms *hk_forms_create(const hk_document *d);
void hk_forms_destroy(hk_forms *f);

/* Current value of a text control (TEXT, PASSWORD, TEXTAREA). */
hk_str hk_forms_value(hk_forms *f, hk_node_id el);
void hk_forms_set_value(hk_forms *f, hk_node_id el, hk_str value);
bool hk_forms_checked(hk_forms *f, hk_node_id el);
/* Checking a radio button unchecks the others of its group. */
void hk_forms_set_checked(hk_forms *f, hk_node_id el, bool checked);
/* Index of the selected option of a <select> (the first selected one when multiple), or -1. */
int hk_forms_selected(hk_forms *f, hk_node_id select);
bool hk_forms_option_selected(hk_forms *f, hk_node_id select, uint32_t index);
/* Selects one option. For a multiple select, toggles it instead. */
void hk_forms_select(hk_forms *f, hk_node_id select, uint32_t index);
/* Restores every control owned by form to its default state. */
void hk_forms_reset(hk_forms *f, hk_node_id form);

/* --- editing -------------------------------------------------------------------------------------- */

typedef enum hk_edit_cmd {
    HK_EDIT_LEFT,
    HK_EDIT_RIGHT,
    HK_EDIT_WORD_LEFT,
    HK_EDIT_WORD_RIGHT,
    HK_EDIT_HOME,
    HK_EDIT_END,
    HK_EDIT_BACKSPACE,
    HK_EDIT_DELETE,
    HK_EDIT_WORD_BACKSPACE,
    HK_EDIT_WORD_DELETE,
    HK_EDIT_SELECT_ALL,
} hk_edit_cmd;

/* The caret and the other end of the selection (equal when nothing is selected). */
void hk_forms_selection(hk_forms *f, hk_node_id el, uint32_t *caret, uint32_t *anchor);
/* Moves the caret to pos (clamped to a code point boundary); extend keeps the anchor. */
void hk_forms_set_caret(hk_forms *f, hk_node_id el, uint32_t pos, bool extend);
/* Applies a command; extend (Shift) extends the selection for movements. Returns true if the value changed. */
bool hk_forms_edit(hk_forms *f, hk_node_id el, hk_edit_cmd cmd, bool extend);
/* Replaces the selection with text (line breaks are removed for single-line controls; maxlength
 * is honoured). Returns true if the value changed. */
bool hk_forms_insert(hk_forms *f, hk_node_id el, hk_str text);
/* The selected text (empty when nothing is selected; always empty for passwords). */
hk_str hk_forms_selected_text(hk_forms *f, hk_node_id el);

/* --- submission ----------------------------------------------------------------------------------- */

typedef struct hk_form_submission {
    hk_charbuf url;  /* the action URL; for GET, with the query replaced by the form data */
    bool post;
    hk_charbuf body; /* POST: application/x-www-form-urlencoded */
} hk_form_submission;

/* Builds the submission of form by submitter (may be none) for a document whose base URL is base
 * and whose encoding is enc. Returns false when the form cannot be submitted (an unsupported
 * action scheme or enctype other than urlencoded falls back to urlencoded). */
bool hk_forms_submit(hk_forms *f, hk_node_id form, hk_node_id submitter, const hk_url *base, hk_encoding enc,
                     hk_form_submission *out);
void hk_form_submission_free(hk_form_submission *s);

/* application/x-www-form-urlencoded byte serialization of one name or value (appends). */
void hk_urlencode_append(hk_charbuf *out, hk_str bytes);

#endif
