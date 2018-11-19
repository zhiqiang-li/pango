/* Pango
 * pangofc-font.c: Shared interfaces for fontconfig-based backends
 *
 * Copyright (C) 2003 Red Hat Software
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:pangofc-font
 * @short_description:Base font class for Fontconfig-based backends
 * @title:PangoFcFont
 * @see_also:
 * <variablelist><varlistentry><term>#PangoFcFontMap</term> <listitem>The base class for font maps; creating a new
 * Fontconfig-based backend involves deriving from both
 * #PangoFcFontMap and #PangoFcFont.</listitem></varlistentry></variablelist>
 *
 * #PangoFcFont is a base class for font implementation using the
 * Fontconfig and FreeType libraries. It is used in the
 * <link linkend="pango-Xft-Fonts-and-Rendering">Xft</link> and
 * <link linkend="pango-FreeType-Fonts-and-Rendering">FreeType</link>
 * backends shipped with Pango, but can also be used when creating
 * new backends. Any backend deriving from this base class will
 * take advantage of the wide range of shapers implemented using
 * FreeType that come with Pango.
 */
#include "config.h"

#include "pangofc-font.h"
#include "pangofc-fontmap.h"
#include "pangofc-private.h"
#include "pango-engine.h"
#include "pango-layout.h"
#include "pango-impl-utils.h"

#include <fontconfig/fcfreetype.h>

#include FT_TRUETYPE_TABLES_H

enum {
  PROP_0,
  PROP_PATTERN,
  PROP_FONTMAP
};

typedef struct _PangoFcFontPrivate PangoFcFontPrivate;

struct _PangoFcFontPrivate
{
  PangoFcDecoder *decoder;
  PangoFcFontKey *key;
  PangoFcCmapCache *cmap_cache;
};

static gboolean pango_fc_font_real_has_char  (PangoFcFont *font,
					      gunichar     wc);
static guint    pango_fc_font_real_get_glyph (PangoFcFont *font,
					      gunichar     wc);

static void                  pango_fc_font_finalize     (GObject          *object);
static void                  pango_fc_font_set_property (GObject          *object,
							 guint             prop_id,
							 const GValue     *value,
							 GParamSpec       *pspec);
static void                  pango_fc_font_get_property (GObject          *object,
							 guint             prop_id,
							 GValue           *value,
							 GParamSpec       *pspec);
static PangoEngineShape *    pango_fc_font_find_shaper  (PangoFont        *font,
							 PangoLanguage    *language,
							 guint32           ch);
static PangoCoverage *       pango_fc_font_get_coverage (PangoFont        *font,
							 PangoLanguage    *language);
static PangoFontMetrics *    pango_fc_font_get_metrics  (PangoFont        *font,
							 PangoLanguage    *language);
static PangoFontMap *        pango_fc_font_get_font_map (PangoFont        *font);
static PangoFontDescription *pango_fc_font_describe     (PangoFont        *font);
static PangoFontDescription *pango_fc_font_describe_absolute (PangoFont        *font);
static hb_font_t *           pango_fc_font_create_hb_font (PangoFont        *font);

#define PANGO_FC_FONT_LOCK_FACE(font)	(PANGO_FC_FONT_GET_CLASS (font)->lock_face (font))
#define PANGO_FC_FONT_UNLOCK_FACE(font)	(PANGO_FC_FONT_GET_CLASS (font)->unlock_face (font))

G_DEFINE_ABSTRACT_TYPE_WITH_CODE (PangoFcFont, pango_fc_font, PANGO_TYPE_FONT,
                                  G_ADD_PRIVATE (PangoFcFont))

static void
pango_fc_font_class_init (PangoFcFontClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);
  PangoFontClass *font_class = PANGO_FONT_CLASS (class);

  class->has_char = pango_fc_font_real_has_char;
  class->get_glyph = pango_fc_font_real_get_glyph;
  class->get_unknown_glyph = NULL;

  object_class->finalize = pango_fc_font_finalize;
  object_class->set_property = pango_fc_font_set_property;
  object_class->get_property = pango_fc_font_get_property;
  font_class->describe = pango_fc_font_describe;
  font_class->describe_absolute = pango_fc_font_describe_absolute;
  font_class->find_shaper = pango_fc_font_find_shaper;
  font_class->get_coverage = pango_fc_font_get_coverage;
  font_class->get_metrics = pango_fc_font_get_metrics;
  font_class->get_font_map = pango_fc_font_get_font_map;
  font_class->create_hb_font = pango_fc_font_create_hb_font;

  g_object_class_install_property (object_class, PROP_PATTERN,
				   g_param_spec_pointer ("pattern",
							 "Pattern",
							 "The fontconfig pattern for this font",
							 G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
							 G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_FONTMAP,
				   g_param_spec_object ("fontmap",
							"Font Map",
							"The PangoFc font map this font is associated with (Since: 1.26)",
							PANGO_TYPE_FC_FONT_MAP,
							G_PARAM_READWRITE |
							G_PARAM_STATIC_STRINGS));
}

static void
pango_fc_font_init (PangoFcFont *fcfont)
{
  fcfont->priv = pango_fc_font_get_instance_private (fcfont);
}

static void
free_metrics_info (PangoFcMetricsInfo *info)
{
  pango_font_metrics_unref (info->metrics);
  g_slice_free (PangoFcMetricsInfo, info);
}

static void
pango_fc_font_finalize (GObject *object)
{
  PangoFcFont *fcfont = PANGO_FC_FONT (object);
  PangoFcFontPrivate *priv = fcfont->priv;
  PangoFcFontMap *fontmap;

  g_slist_foreach (fcfont->metrics_by_lang, (GFunc)free_metrics_info, NULL);
  g_slist_free (fcfont->metrics_by_lang);

  fontmap = g_weak_ref_get ((GWeakRef *) &fcfont->fontmap);
  if (fontmap)
    {
      _pango_fc_font_map_remove (PANGO_FC_FONT_MAP (fcfont->fontmap), fcfont);
      g_weak_ref_clear ((GWeakRef *) &fcfont->fontmap);
      g_object_unref (fontmap);
    }

  FcPatternDestroy (fcfont->font_pattern);
  pango_font_description_free (fcfont->description);

  if (priv->decoder)
    _pango_fc_font_set_decoder (fcfont, NULL);

  if (priv->cmap_cache)
    _pango_fc_cmap_cache_unref (priv->cmap_cache);

  G_OBJECT_CLASS (pango_fc_font_parent_class)->finalize (object);
}

static gboolean
pattern_is_hinted (FcPattern *pattern)
{
  FcBool hinting;

  if (FcPatternGetBool (pattern,
			FC_HINTING, 0, &hinting) != FcResultMatch)
    hinting = FcTrue;

  return hinting;
}

static gboolean
pattern_is_transformed (FcPattern *pattern)
{
  FcMatrix *fc_matrix;

  if (FcPatternGetMatrix (pattern, FC_MATRIX, 0, &fc_matrix) == FcResultMatch)
    {
      FT_Matrix ft_matrix;

      ft_matrix.xx = 0x10000L * fc_matrix->xx;
      ft_matrix.yy = 0x10000L * fc_matrix->yy;
      ft_matrix.xy = 0x10000L * fc_matrix->xy;
      ft_matrix.yx = 0x10000L * fc_matrix->yx;

      return ((ft_matrix.xy | ft_matrix.yx) != 0 ||
	      ft_matrix.xx != 0x10000L ||
	      ft_matrix.yy != 0x10000L);
    }
  else
    return FALSE;
}

static void
pango_fc_font_set_property (GObject       *object,
			    guint          prop_id,
			    const GValue  *value,
			    GParamSpec    *pspec)
{
  PangoFcFont *fcfont = PANGO_FC_FONT (object);

  switch (prop_id)
    {
    case PROP_PATTERN:
      {
	FcPattern *pattern = g_value_get_pointer (value);

	g_return_if_fail (pattern != NULL);
	g_return_if_fail (fcfont->font_pattern == NULL);

	FcPatternReference (pattern);
	fcfont->font_pattern = pattern;
	fcfont->description = pango_fc_font_description_from_pattern (pattern, TRUE);
	fcfont->is_hinted = pattern_is_hinted (pattern);
	fcfont->is_transformed = pattern_is_transformed (pattern);
      }
      goto set_decoder;

    case PROP_FONTMAP:
      {
	PangoFcFontMap *fcfontmap = PANGO_FC_FONT_MAP (g_value_get_object (value));

	g_return_if_fail (fcfont->fontmap == NULL);
	g_weak_ref_set ((GWeakRef *) &fcfont->fontmap, fcfontmap);
      }
      goto set_decoder;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      return;
    }

set_decoder:
  /* set decoder if both pattern and fontmap are set now */
  if (fcfont->font_pattern && fcfont->fontmap)
    _pango_fc_font_set_decoder (fcfont,
				pango_fc_font_map_find_decoder  ((PangoFcFontMap *) fcfont->fontmap,
								 fcfont->font_pattern));
}

static void
pango_fc_font_get_property (GObject       *object,
			    guint          prop_id,
			    GValue        *value,
			    GParamSpec    *pspec)
{
  switch (prop_id)
    {
    case PROP_PATTERN:
      {
	PangoFcFont *fcfont = PANGO_FC_FONT (object);
	g_value_set_pointer (value, fcfont->font_pattern);
      }
      break;
    case PROP_FONTMAP:
      {
	PangoFcFont *fcfont = PANGO_FC_FONT (object);
	PangoFontMap *fontmap = g_weak_ref_get ((GWeakRef *) &fcfont->fontmap);
	g_value_take_object (value, fontmap);
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static PangoFontDescription *
pango_fc_font_describe (PangoFont *font)
{
  PangoFcFont *fcfont = (PangoFcFont *)font;

  return pango_font_description_copy (fcfont->description);
}

static PangoFontDescription *
pango_fc_font_describe_absolute (PangoFont *font)
{
  PangoFcFont *fcfont = (PangoFcFont *)font;
  PangoFontDescription *desc = pango_font_description_copy (fcfont->description);
  double size;

  if (FcPatternGetDouble (fcfont->font_pattern, FC_PIXEL_SIZE, 0, &size) == FcResultMatch)
    pango_font_description_set_absolute_size (desc, size * PANGO_SCALE);

  return desc;
}

/* Wrap shaper in PangoEngineShape to pass it through old API,
 * from times when there were modules and engines. */
typedef PangoEngineShape      PangoFcShapeEngine;
typedef PangoEngineShapeClass PangoFcShapeEngineClass;
static GType pango_fc_shape_engine_get_type (void) G_GNUC_CONST;
G_DEFINE_TYPE (PangoFcShapeEngine, pango_fc_shape_engine, PANGO_TYPE_ENGINE_SHAPE);
static void
_pango_fc_shape_engine_shape (PangoEngineShape    *engine G_GNUC_UNUSED,
			      PangoFont           *font,
			      const char          *item_text,
			      unsigned int         item_length,
			      const PangoAnalysis *analysis,
			      PangoGlyphString    *glyphs,
			      const char          *paragraph_text,
			      unsigned int         paragraph_length)
{
  _pango_fc_shape (font, item_text, item_length, analysis, glyphs,
		   paragraph_text, paragraph_length);
}
static void
pango_fc_shape_engine_class_init (PangoEngineShapeClass *class)
{
  class->script_shape = _pango_fc_shape_engine_shape;
}
static void
pango_fc_shape_engine_init (PangoEngineShape *object)
{
}

static PangoEngineShape *
pango_fc_font_find_shaper (PangoFont     *font G_GNUC_UNUSED,
			   PangoLanguage *language,
			   guint32        ch)
{
  static PangoEngineShape *shaper;
  if (g_once_init_enter (&shaper))
    g_once_init_leave (&shaper, g_object_new (pango_fc_shape_engine_get_type(), NULL));
  return shaper;
}

static PangoCoverage *
pango_fc_font_get_coverage (PangoFont     *font,
			    PangoLanguage *language G_GNUC_UNUSED)
{
  PangoFcFont *fcfont = (PangoFcFont *)font;
  PangoFcFontPrivate *priv = fcfont->priv;
  FcCharSet *charset;
  PangoFcFontMap *fontmap;
  PangoCoverage *coverage;

  if (priv->decoder)
    {
      charset = pango_fc_decoder_get_charset (priv->decoder, fcfont);
      return _pango_fc_font_map_fc_to_coverage (charset);
    }

  fontmap = g_weak_ref_get ((GWeakRef *) &fcfont->fontmap);
  if (!fontmap)
    return pango_coverage_new ();

  coverage = _pango_fc_font_map_get_coverage (fontmap, fcfont);
  g_object_unref (fontmap);
  return coverage;
}

/* For Xft, it would be slightly more efficient to simply to
 * call Xft, and also more robust against changes in Xft.
 * But for now, we simply use the same code for all backends.
 *
 * The code in this function is partly based on code from Xft,
 * Copyright 2000 Keith Packard
 */
static void
get_face_metrics (PangoFcFont      *fcfont,
		  PangoFontMetrics *metrics)
{
  FT_Face face = PANGO_FC_FONT_LOCK_FACE (fcfont);
  FcMatrix *fc_matrix;
  FT_Matrix ft_matrix;
  TT_OS2 *os2;
  gboolean have_transform = FALSE;

  if (G_UNLIKELY (!face))
    {
      metrics->descent = 0;
      metrics->ascent = PANGO_SCALE * PANGO_UNKNOWN_GLYPH_HEIGHT;
      metrics->underline_thickness = PANGO_SCALE;
      metrics->underline_position = - PANGO_SCALE;
      metrics->strikethrough_thickness = PANGO_SCALE;
      metrics->strikethrough_position = PANGO_SCALE * (PANGO_UNKNOWN_GLYPH_HEIGHT/2);
      return;
    }

  if  (FcPatternGetMatrix (fcfont->font_pattern,
			   FC_MATRIX, 0, &fc_matrix) == FcResultMatch)
    {
      ft_matrix.xx = 0x10000L * fc_matrix->xx;
      ft_matrix.yy = 0x10000L * fc_matrix->yy;
      ft_matrix.xy = 0x10000L * fc_matrix->xy;
      ft_matrix.yx = 0x10000L * fc_matrix->yx;

      have_transform = (ft_matrix.xx != 0x10000 || ft_matrix.xy != 0 ||
			ft_matrix.yx != 0 || ft_matrix.yy != 0x10000);
    }

  if (have_transform)
    {
      FT_Vector	vector;

      vector.x = 0;
      vector.y = face->size->metrics.descender;
      FT_Vector_Transform (&vector, &ft_matrix);
      metrics->descent = - PANGO_UNITS_26_6 (vector.y);

      vector.x = 0;
      vector.y = face->size->metrics.ascender;
      FT_Vector_Transform (&vector, &ft_matrix);
      metrics->ascent = PANGO_UNITS_26_6 (vector.y);
    }
  else if (fcfont->is_hinted ||
	   (face->face_flags & FT_FACE_FLAG_SCALABLE) == 0)
    {
      metrics->descent = - PANGO_UNITS_26_6 (face->size->metrics.descender);
      metrics->ascent = PANGO_UNITS_26_6 (face->size->metrics.ascender);
    }
  else
    {
      FT_Fixed ascender, descender;

      descender = FT_MulFix (face->descender, face->size->metrics.y_scale);
      metrics->descent = - PANGO_UNITS_26_6 (descender);

      ascender = FT_MulFix (face->ascender, face->size->metrics.y_scale);
      metrics->ascent = PANGO_UNITS_26_6 (ascender);
    }


  metrics->underline_thickness = 0;
  metrics->underline_position = 0;

  {
    FT_Fixed ft_thickness, ft_position;

    ft_thickness = FT_MulFix (face->underline_thickness, face->size->metrics.y_scale);
    metrics->underline_thickness = PANGO_UNITS_26_6 (ft_thickness);

    ft_position = FT_MulFix (face->underline_position, face->size->metrics.y_scale);
    metrics->underline_position = PANGO_UNITS_26_6 (ft_position);
  }

  if (metrics->underline_thickness == 0 || metrics->underline_position == 0)
    {
      metrics->underline_thickness = (PANGO_SCALE * face->size->metrics.y_ppem) / 14;
      metrics->underline_position = - metrics->underline_thickness;
    }


  metrics->strikethrough_thickness = 0;
  metrics->strikethrough_position = 0;

  os2 = FT_Get_Sfnt_Table (face, ft_sfnt_os2);
  if (os2 && os2->version != 0xFFFF)
    {
      FT_Fixed ft_thickness, ft_position;

      ft_thickness = FT_MulFix (os2->yStrikeoutSize, face->size->metrics.y_scale);
      metrics->strikethrough_thickness = PANGO_UNITS_26_6 (ft_thickness);

      ft_position = FT_MulFix (os2->yStrikeoutPosition, face->size->metrics.y_scale);
      metrics->strikethrough_position = PANGO_UNITS_26_6 (ft_position);
    }

  if (metrics->strikethrough_thickness == 0 || metrics->strikethrough_position == 0)
    {
      metrics->strikethrough_thickness = metrics->underline_thickness;
      metrics->strikethrough_position = (PANGO_SCALE * face->size->metrics.y_ppem) / 4;
    }


  /* If hinting is on for this font, quantize the underline and strikethrough position
   * to integer values.
   */
  if (fcfont->is_hinted)
    {
      pango_quantize_line_geometry (&metrics->underline_thickness,
				    &metrics->underline_position);
      pango_quantize_line_geometry (&metrics->strikethrough_thickness,
				    &metrics->strikethrough_position);

      /* Quantizing may have pushed underline_position to 0.  Not good */
      if (metrics->underline_position == 0)
	metrics->underline_position = - metrics->underline_thickness;
    }


  PANGO_FC_FONT_UNLOCK_FACE (fcfont);
}

PangoFontMetrics *
pango_fc_font_create_base_metrics_for_context (PangoFcFont   *fcfont,
					       PangoContext  *context)
{
  PangoFontMetrics *metrics;
  metrics = pango_font_metrics_new ();

  get_face_metrics (fcfont, metrics);

  return metrics;
}

static int
max_glyph_width (PangoLayout *layout)
{
  int max_width = 0;
  GSList *l, *r;

  for (l = pango_layout_get_lines_readonly (layout); l; l = l->next)
    {
      PangoLayoutLine *line = l->data;

      for (r = line->runs; r; r = r->next)
	{
	  PangoGlyphString *glyphs = ((PangoGlyphItem *)r->data)->glyphs;
	  int i;

	  for (i = 0; i < glyphs->num_glyphs; i++)
	    if (glyphs->glyphs[i].geometry.width > max_width)
	      max_width = glyphs->glyphs[i].geometry.width;
	}
    }

  return max_width;
}

static PangoFontMetrics *
pango_fc_font_get_metrics (PangoFont     *font,
			   PangoLanguage *language)
{
  PangoFcFont *fcfont = PANGO_FC_FONT (font);
  PangoFcMetricsInfo *info = NULL; /* Quiet gcc */
  GSList *tmp_list;

  const char *sample_str = pango_language_get_sample_string (language);

  tmp_list = fcfont->metrics_by_lang;
  while (tmp_list)
    {
      info = tmp_list->data;

      if (info->sample_str == sample_str)    /* We _don't_ need strcmp */
	break;

      tmp_list = tmp_list->next;
    }

  if (!tmp_list)
    {
      PangoFontMap *fontmap;
      PangoContext *context;

      fontmap = g_weak_ref_get ((GWeakRef *) &fcfont->fontmap);
      if (!fontmap)
	return pango_font_metrics_new ();

      info = g_slice_new0 (PangoFcMetricsInfo);

      fcfont->metrics_by_lang = g_slist_prepend (fcfont->metrics_by_lang,
						 info);

      info->sample_str = sample_str;

      context = pango_font_map_create_context (fontmap);
      pango_context_set_language (context, language);

      info->metrics = pango_fc_font_create_base_metrics_for_context (fcfont, context);

      { /* Compute derived metrics */
	PangoLayout *layout;
	PangoRectangle extents;
	const char *sample_str = pango_language_get_sample_string (language);
	PangoFontDescription *desc = pango_font_describe_with_absolute_size (font);
	gulong sample_str_width;

        layout = pango_layout_new (context);
	pango_layout_set_font_description (layout, desc);
	pango_font_description_free (desc);

	pango_layout_set_text (layout, sample_str, -1);
	pango_layout_get_extents (layout, NULL, &extents);

	sample_str_width = pango_utf8_strwidth (sample_str);
	g_assert (sample_str_width > 0);
	info->metrics->approximate_char_width = extents.width / sample_str_width;

	pango_layout_set_text (layout, "0123456789", -1);
	info->metrics->approximate_digit_width = max_glyph_width (layout);

	g_object_unref (layout);
      }

      g_object_unref (context);
      g_object_unref (fontmap);
    }

  return pango_font_metrics_ref (info->metrics);
}

static PangoFontMap *
pango_fc_font_get_font_map (PangoFont *font)
{
  PangoFcFont *fcfont = PANGO_FC_FONT (font);

  /* MT-unsafe.  Oh well...  The API is unsafe. */
  return fcfont->fontmap;
}

static gboolean
pango_fc_font_real_has_char (PangoFcFont *font,
			     gunichar     wc)
{
  FcCharSet *charset;

  if (FcPatternGetCharSet (font->font_pattern,
			   FC_CHARSET, 0, &charset) != FcResultMatch)
    return FALSE;

  return FcCharSetHasChar (charset, wc);
}

static guint
pango_fc_font_real_get_glyph (PangoFcFont *font,
			      gunichar     wc)
{
  PangoFcFontPrivate *priv = font->priv;
  FT_Face face;
  FT_UInt index;

  guint idx;
  PangoFcCmapCacheEntry *entry;


  if (G_UNLIKELY (priv->cmap_cache == NULL))
    {
      PangoFcFontMap *fontmap = g_weak_ref_get ((GWeakRef *) &font->fontmap);
      if (G_UNLIKELY (!fontmap))
        return 0;

      priv->cmap_cache = _pango_fc_font_map_get_cmap_cache (fontmap, font);

      g_object_unref (fontmap);

      if (G_UNLIKELY (!priv->cmap_cache))
	return 0;
    }

  idx = wc & CMAP_CACHE_MASK;
  entry = priv->cmap_cache->entries + idx;

  if (entry->ch != wc)
    {
      face = PANGO_FC_FONT_LOCK_FACE (font);
      if (G_LIKELY (face))
        {
	  index = FcFreeTypeCharIndex (face, wc);
	  if (index > (FT_UInt)face->num_glyphs)
	    index = 0;

	  PANGO_FC_FONT_UNLOCK_FACE (font);
	}
      else
        index = 0;

      entry->ch = wc;
      entry->glyph = index;
    }

  return entry->glyph;
}

/**
 * pango_fc_font_lock_face:
 * @font: a #PangoFcFont.
 *
 * Gets the FreeType <type>FT_Face</type> associated with a font,
 * This face will be kept around until you call
 * pango_fc_font_unlock_face().
 *
 * Return value: the FreeType <type>FT_Face</type> associated with @font.
 *
 * Since: 1.4
 **/
FT_Face
pango_fc_font_lock_face (PangoFcFont *font)
{
  g_return_val_if_fail (PANGO_IS_FC_FONT (font), NULL);

  return PANGO_FC_FONT_LOCK_FACE (font);
}

/**
 * pango_fc_font_unlock_face:
 * @font: a #PangoFcFont.
 *
 * Releases a font previously obtained with
 * pango_fc_font_lock_face().
 *
 * Since: 1.4
 **/
void
pango_fc_font_unlock_face (PangoFcFont *font)
{
  g_return_if_fail (PANGO_IS_FC_FONT (font));

  PANGO_FC_FONT_UNLOCK_FACE (font);
}

/**
 * pango_fc_font_has_char:
 * @font: a #PangoFcFont
 * @wc: Unicode codepoint to look up
 *
 * Determines whether @font has a glyph for the codepoint @wc.
 *
 * Return value: %TRUE if @font has the requested codepoint.
 *
 * Since: 1.4
 **/
gboolean
pango_fc_font_has_char (PangoFcFont *font,
			gunichar     wc)
{
  PangoFcFontPrivate *priv = font->priv;
  FcCharSet *charset;

  g_return_val_if_fail (PANGO_IS_FC_FONT (font), FALSE);

  if (priv->decoder)
    {
      charset = pango_fc_decoder_get_charset (priv->decoder, font);
      return FcCharSetHasChar (charset, wc);
    }

  return PANGO_FC_FONT_GET_CLASS (font)->has_char (font, wc);
}

/**
 * pango_fc_font_get_glyph:
 * @font: a #PangoFcFont
 * @wc: Unicode character to look up
 *
 * Gets the glyph index for a given Unicode character
 * for @font. If you only want to determine
 * whether the font has the glyph, use pango_fc_font_has_char().
 *
 * Return value: the glyph index, or 0, if the Unicode
 *   character doesn't exist in the font.
 *
 * Since: 1.4
 **/
PangoGlyph
pango_fc_font_get_glyph (PangoFcFont *font,
			 gunichar     wc)
{
  PangoFcFontPrivate *priv = font->priv;

  /* Replace NBSP with a normal space; it should be invariant that
   * they shape the same other than breaking properties.
   */
  if (wc == 0xA0)
	  wc = 0x20;

  if (priv->decoder)
    return pango_fc_decoder_get_glyph (priv->decoder, font, wc);

  return PANGO_FC_FONT_GET_CLASS (font)->get_glyph (font, wc);
}


/**
 * pango_fc_font_get_unknown_glyph:
 * @font: a #PangoFcFont
 * @wc: the Unicode character for which a glyph is needed.
 *
 * Returns the index of a glyph suitable for drawing @wc as an
 * unknown character.
 *
 * Use PANGO_GET_UNKNOWN_GLYPH() instead.
 *
 * Return value: a glyph index into @font.
 *
 * Since: 1.4
 **/
PangoGlyph
pango_fc_font_get_unknown_glyph (PangoFcFont *font,
				 gunichar     wc)
{
  if (font && PANGO_FC_FONT_GET_CLASS (font)->get_unknown_glyph)
    return PANGO_FC_FONT_GET_CLASS (font)->get_unknown_glyph (font, wc);

  return PANGO_GET_UNKNOWN_GLYPH (wc);
}

void
_pango_fc_font_shutdown (PangoFcFont *font)
{
  g_return_if_fail (PANGO_IS_FC_FONT (font));

  if (PANGO_FC_FONT_GET_CLASS (font)->shutdown)
    PANGO_FC_FONT_GET_CLASS (font)->shutdown (font);
}

/**
 * pango_fc_font_kern_glyphs:
 * @font: a #PangoFcFont
 * @glyphs: a #PangoGlyphString
 *
 * Adjust each adjacent pair of glyphs in @glyphs according to
 * kerning information in @font.
 *
 * Since: 1.4
 * Deprecated: 1.32
 **/
void
pango_fc_font_kern_glyphs (PangoFcFont      *font,
			   PangoGlyphString *glyphs)
{
  FT_Face face;
  FT_Error error;
  FT_Vector kerning;
  int i;
  gboolean hinting = font->is_hinted;
  gboolean scale = FALSE;
  double xscale = 1;
  PangoFcFontKey *key;

  g_return_if_fail (PANGO_IS_FC_FONT (font));
  g_return_if_fail (glyphs != NULL);

  face = PANGO_FC_FONT_LOCK_FACE (font);
  if (G_UNLIKELY (!face))
    return;

  if (!FT_HAS_KERNING (face))
    {
      PANGO_FC_FONT_UNLOCK_FACE (font);
      return;
    }

  key = _pango_fc_font_get_font_key (font);
  if (key) {
    const PangoMatrix *matrix = pango_fc_font_key_get_matrix (key);
    PangoMatrix identity = PANGO_MATRIX_INIT;
    if (G_UNLIKELY (matrix && 0 != memcmp (&identity, matrix, 2 * sizeof (double))))
      {
	scale = TRUE;
	pango_matrix_get_font_scale_factors (matrix, &xscale, NULL);
	if (xscale) xscale = 1 / xscale;
      }
  }

  for (i = 1; i < glyphs->num_glyphs; ++i)
    {
      error = FT_Get_Kerning (face,
			      glyphs->glyphs[i-1].glyph,
			      glyphs->glyphs[i].glyph,
			      ft_kerning_default,
			      &kerning);

      if (error == FT_Err_Ok) {
	int adjustment = PANGO_UNITS_26_6 (kerning.x);

	if (hinting)
	  adjustment = PANGO_UNITS_ROUND (adjustment);
	if (G_UNLIKELY (scale))
	  adjustment *= xscale;

	glyphs->glyphs[i-1].geometry.width += adjustment;
      }
    }

  PANGO_FC_FONT_UNLOCK_FACE (font);
}

/**
 * _pango_fc_font_get_decoder:
 * @font: a #PangoFcFont
 *
 * This will return any custom decoder set on this font.
 *
 * Return value: The custom decoder
 *
 * Since: 1.6
 **/

PangoFcDecoder *
_pango_fc_font_get_decoder (PangoFcFont *font)
{
  PangoFcFontPrivate *priv = font->priv;

  return priv->decoder;
}

/**
 * _pango_fc_font_set_decoder:
 * @font: a #PangoFcFont
 * @decoder: a #PangoFcDecoder to set for this font
 *
 * This sets a custom decoder for this font.  Any previous decoder
 * will be released before this one is set.
 *
 * Since: 1.6
 **/

void
_pango_fc_font_set_decoder (PangoFcFont    *font,
			    PangoFcDecoder *decoder)
{
  PangoFcFontPrivate *priv = font->priv;

  if (priv->decoder)
    g_object_unref (priv->decoder);

  priv->decoder = decoder;

  if (priv->decoder)
    g_object_ref (priv->decoder);
}

PangoFcFontKey *
_pango_fc_font_get_font_key (PangoFcFont *fcfont)
{
  PangoFcFontPrivate *priv = fcfont->priv;

  return priv->key;
}

void
_pango_fc_font_set_font_key (PangoFcFont    *fcfont,
			     PangoFcFontKey *key)
{
  PangoFcFontPrivate *priv = fcfont->priv;

  priv->key = key;
}

static FT_Glyph_Metrics *
get_per_char (FT_Face      face,
	      FT_Int32     load_flags,
	      PangoGlyph   glyph)
{
  FT_Error error;
  FT_Glyph_Metrics *result;

  error = FT_Load_Glyph (face, glyph, load_flags);
  if (error == FT_Err_Ok)
    result = &face->glyph->metrics;
  else
    result = NULL;

  return result;
}

/**
 * pango_fc_font_get_raw_extents:
 * @fcfont: a #PangoFcFont
 * @load_flags: flags to pass to FT_Load_Glyph()
 * @glyph: the glyph index to load
 * @ink_rect: (out) (optional): location to store ink extents of the
 *   glyph, or %NULL
 * @logical_rect: (out) (optional): location to store logical extents
 *   of the glyph or %NULL
 *
 * Gets the extents of a single glyph from a font. The extents are in
 * user space; that is, they are not transformed by any matrix in effect
 * for the font.
 *
 * Long term, this functionality probably belongs in the default
 * implementation of the get_glyph_extents() virtual function.
 * The other possibility would be to to make it public in something
 * like it's current form, and also expose glyph information
 * caching functionality similar to pango_ft2_font_set_glyph_info().
 *
 * Since: 1.6
 **/
void
pango_fc_font_get_raw_extents (PangoFcFont    *fcfont,
			       FT_Int32        load_flags,
			       PangoGlyph      glyph,
			       PangoRectangle *ink_rect,
			       PangoRectangle *logical_rect)
{
  FT_Glyph_Metrics *gm;
  FT_Face face;

  g_return_if_fail (PANGO_IS_FC_FONT (fcfont));

  face = PANGO_FC_FONT_LOCK_FACE (fcfont);
  if (G_UNLIKELY (!face))
    {
      /* Get generic unknown-glyph extents. */
      pango_font_get_glyph_extents (NULL, glyph, ink_rect, logical_rect);
      return;
    }

  if (glyph == PANGO_GLYPH_EMPTY)
    gm = NULL;
  else
    gm = get_per_char (face, load_flags, glyph);

  if (gm)
    {
      if (ink_rect)
	{
	  ink_rect->x = PANGO_UNITS_26_6 (gm->horiBearingX);
	  ink_rect->width = PANGO_UNITS_26_6 (gm->width);
	  ink_rect->y = -PANGO_UNITS_26_6 (gm->horiBearingY);
	  ink_rect->height = PANGO_UNITS_26_6 (gm->height);
	}

      if (logical_rect)
	{
	  logical_rect->x = 0;
	  logical_rect->width = PANGO_UNITS_26_6 (gm->horiAdvance);
	  if (fcfont->is_hinted ||
	      (face->face_flags & FT_FACE_FLAG_SCALABLE) == 0)
	    {
	      logical_rect->y = - PANGO_UNITS_26_6 (face->size->metrics.ascender);
	      logical_rect->height = PANGO_UNITS_26_6 (face->size->metrics.ascender - face->size->metrics.descender);
	    }
	  else
	    {
	      FT_Fixed ascender, descender;

	      ascender = FT_MulFix (face->ascender, face->size->metrics.y_scale);
	      descender = FT_MulFix (face->descender, face->size->metrics.y_scale);

	      logical_rect->y = - PANGO_UNITS_26_6 (ascender);
	      logical_rect->height = PANGO_UNITS_26_6 (ascender - descender);
	    }
	}
    }
  else
    {
      if (ink_rect)
	{
	  ink_rect->x = 0;
	  ink_rect->width = 0;
	  ink_rect->y = 0;
	  ink_rect->height = 0;
	}

      if (logical_rect)
	{
	  logical_rect->x = 0;
	  logical_rect->width = 0;
	  logical_rect->y = 0;
	  logical_rect->height = 0;
	}
    }

  PANGO_FC_FONT_UNLOCK_FACE (fcfont);
}

typedef struct _PangoFcHbContext {
  FT_Face ft_face;
  PangoFcFont *fc_font;
  gboolean vertical;
  double x_scale, y_scale; /* CTM scales. */
} PangoFcHbContext;

static hb_bool_t
pango_fc_hb_font_get_nominal_glyph (hb_font_t *font, void *font_data,
                                    hb_codepoint_t unicode,
                                    hb_codepoint_t *glyph,
                                    void *user_data G_GNUC_UNUSED)
{
  PangoFcHbContext *context = (PangoFcHbContext *) font_data;
  PangoFcFont *fc_font = context->fc_font;

  *glyph = pango_fc_font_get_glyph (fc_font, unicode);
  if (G_LIKELY (*glyph))
    return TRUE;

  *glyph = PANGO_GET_UNKNOWN_GLYPH (unicode);

  /* We draw our own invalid-Unicode shape, so prevent HarfBuzz
   * from using REPLACEMENT CHARACTER. */
  if (unicode > 0x10FFFF)
    return TRUE;

  return FALSE;
}

static hb_bool_t
pango_fc_hb_font_get_variation_glyph (hb_font_t *font,
                                      void *font_data,
                                      hb_codepoint_t unicode,
                                      hb_codepoint_t variation_selector,
                                      hb_codepoint_t *glyph,
                                      void *user_data G_GNUC_UNUSED)
{
  PangoFcHbContext *context = (PangoFcHbContext *) font_data;
  FT_Face ft_face = context->ft_face;
  unsigned int g;

  g = FT_Face_GetCharVariantIndex (ft_face, unicode, variation_selector);

  if (G_UNLIKELY (!g))
    return FALSE;

  *glyph = g;
  return TRUE;
}

static hb_bool_t
pango_fc_hb_font_get_glyph_contour_point (hb_font_t *font, void *font_data,
                                          hb_codepoint_t glyph, unsigned int point_index,
                                          hb_position_t *x, hb_position_t *y,
                                          void *user_data G_GNUC_UNUSED)
{
  return FALSE;
#if 0
  FT_Face ft_face = (FT_Face) font_data;
  int load_flags = FT_LOAD_DEFAULT;

  /* TODO: load_flags, embolden, etc */

  if (HB_UNLIKELY (FT_Load_Glyph (ft_face, glyph, load_flags)))
      return FALSE;

  if (HB_UNLIKELY (ft_face->glyph->format != FT_GLYPH_FORMAT_OUTLINE))
      return FALSE;

  if (HB_UNLIKELY (point_index >= (unsigned int) ft_face->glyph->outline.n_points))
      return FALSE;

  *x = ft_face->glyph->outline.points[point_index].x;
  *y = ft_face->glyph->outline.points[point_index].y;

  return TRUE;
#endif
}

static hb_position_t
pango_fc_hb_font_get_glyph_advance (hb_font_t *font, void *font_data,
                                    hb_codepoint_t glyph,
                                    void *user_data G_GNUC_UNUSED)
{
  PangoFcHbContext *context = (PangoFcHbContext *) font_data;
  PangoFcFont *fc_font = context->fc_font;
  PangoRectangle logical;

  pango_font_get_glyph_extents ((PangoFont *) fc_font, glyph, NULL, &logical);

  return logical.width;
}

static hb_bool_t
pango_fc_hb_font_get_glyph_extents (hb_font_t *font,  void *font_data,
                                    hb_codepoint_t glyph,
                                    hb_glyph_extents_t *extents,
                                    void *user_data G_GNUC_UNUSED)
{
  PangoFcHbContext *context = (PangoFcHbContext *) font_data;
  PangoFcFont *fc_font = context->fc_font;
  PangoRectangle ink;

  pango_font_get_glyph_extents ((PangoFont *) fc_font, glyph, &ink, NULL);

  if (G_LIKELY (!context->vertical)) {
    extents->x_bearing  = ink.x;
    extents->y_bearing  = ink.y;
    extents->width      = ink.width;
    extents->height     = ink.height;
  } else {
    /* XXX */
    extents->x_bearing  = ink.x;
    extents->y_bearing  = ink.y;
    extents->width      = ink.height;
    extents->height     = ink.width;
  }

  return TRUE;
}

static hb_bool_t
pango_fc_hb_font_get_glyph_h_origin (hb_font_t *font, void *font_data,
                                     hb_codepoint_t glyph,
                                     hb_position_t *x, hb_position_t *y,
                                     void *user_data G_GNUC_UNUSED)
{
  PangoFcHbContext *context = (PangoFcHbContext *) font_data;
  FT_Face ft_face = context->ft_face;
  int load_flags = FT_LOAD_DEFAULT;

  if (!context->vertical) return TRUE;

  if (FT_Load_Glyph (ft_face, glyph, load_flags))
    return FALSE;

  /* Note: FreeType's vertical metrics grows downward while other FreeType coordinates
   * have a Y growing upward.  Hence the extra negations. */
  *x = -PANGO_UNITS_26_6 (ft_face->glyph->metrics.horiBearingX -   ft_face->glyph->metrics.vertBearingX);
  *y = +PANGO_UNITS_26_6 (ft_face->glyph->metrics.horiBearingY - (-ft_face->glyph->metrics.vertBearingY));

  return TRUE;
}

static hb_bool_t
pango_fc_hb_font_get_glyph_v_origin (hb_font_t *font, void *font_data,
                                     hb_codepoint_t glyph,
                                     hb_position_t *x, hb_position_t *y,
                                     void *user_data G_GNUC_UNUSED)
{
  PangoFcHbContext *context = (PangoFcHbContext *) font_data;
  FT_Face ft_face = context->ft_face;
  int load_flags = FT_LOAD_DEFAULT;

  /* pangocairo-fc configures font in vertical origin for vertical writing. */
  if (context->vertical) return TRUE;

  if (FT_Load_Glyph (ft_face, glyph, load_flags))
    return FALSE;

  /* Note: FreeType's vertical metrics grows downward while other FreeType coordinates
   * have a Y growing upward.  Hence the extra negation. */
  *x = PANGO_UNITS_26_6 (ft_face->glyph->metrics.horiBearingX -   ft_face->glyph->metrics.vertBearingX);
  *y = PANGO_UNITS_26_6 (ft_face->glyph->metrics.horiBearingY - (-ft_face->glyph->metrics.vertBearingY));

  /* XXX */

  return TRUE;
}

static hb_position_t
pango_fc_hb_font_get_h_kerning (hb_font_t *font, void *font_data,
                                hb_codepoint_t left_glyph, hb_codepoint_t right_glyph,
                                void *user_data G_GNUC_UNUSED)
{
  PangoFcHbContext *context = (PangoFcHbContext *) font_data;
  FT_Face ft_face = context->ft_face;
  FT_Vector kerning;

  if (FT_Get_Kerning (ft_face, left_glyph, right_glyph, FT_KERNING_DEFAULT, &kerning))
    return 0;

  return PANGO_UNITS_26_6 (kerning.x * context->x_scale);
}

static hb_font_funcs_t *
pango_fc_get_hb_font_funcs (void)
{
  static hb_font_funcs_t *funcs;

  if (G_UNLIKELY (!funcs)) {
    funcs = hb_font_funcs_create ();
    hb_font_funcs_set_nominal_glyph_func (funcs, pango_fc_hb_font_get_nominal_glyph, NULL, NULL);
    hb_font_funcs_set_variation_glyph_func (funcs, pango_fc_hb_font_get_variation_glyph, NULL, NULL);
    hb_font_funcs_set_glyph_h_advance_func (funcs, pango_fc_hb_font_get_glyph_advance, NULL, NULL);
    hb_font_funcs_set_glyph_v_advance_func (funcs, pango_fc_hb_font_get_glyph_advance, NULL, NULL);
    hb_font_funcs_set_glyph_h_origin_func (funcs, pango_fc_hb_font_get_glyph_h_origin, NULL, NULL);
    hb_font_funcs_set_glyph_v_origin_func (funcs, pango_fc_hb_font_get_glyph_v_origin, NULL, NULL);
    hb_font_funcs_set_glyph_h_kerning_func (funcs, pango_fc_hb_font_get_h_kerning, NULL, NULL);
    /* Don't need v_kerning. */
    hb_font_funcs_set_glyph_extents_func (funcs, pango_fc_hb_font_get_glyph_extents, NULL, NULL);
    hb_font_funcs_set_glyph_contour_point_func (funcs, pango_fc_hb_font_get_glyph_contour_point, NULL, NULL);
    /* Don't need glyph_name / glyph_from_name */
  }

  return funcs;
}

extern gpointer get_gravity_class (void);

static PangoGravity
pango_fc_font_key_get_gravity (PangoFcFontKey *key)
{
  FcPattern *pattern;
  PangoGravity gravity = PANGO_GRAVITY_SOUTH;
  FcChar8 *s;

  pattern = pango_fc_font_key_get_pattern (key);
  if (FcPatternGetString (pattern, PANGO_FC_GRAVITY, 0, (FcChar8 **)&s) == FcResultMatch)
    {
      GEnumValue *value = g_enum_get_value_by_nick (get_gravity_class (), (char *)s);
      gravity = value->value;
    }

  return gravity;
}

static void
parse_variations (const char      *variations,
                  hb_variation_t **hb_variations,
                  guint           *n_variations)
{
  guint n;
  hb_variation_t *var;
  int i;
  const char *p;

  n = 1;
  for (i = 0; variations[i]; i++)
    {
      if (variations[i] == ',')
        n++;
    }

  var = g_new (hb_variation_t, n);

  p = variations;
  n = 0;
  while (p && *p)
    {
      char *end = strchr (p, ',');
      if (hb_variation_from_string (p, end ? end - p: -1, &var[n]))
        n++;
      p = end ? end + 1 : NULL;
    }

  *hb_variations = var;
  *n_variations = n;
}

static hb_font_t *
pango_fc_font_create_hb_font (PangoFont *font)
{
  PangoFcFont *fc_font = PANGO_FC_FONT (font);
  PangoFcFontKey *key;
  PangoFcHbContext context;
  FT_Face ft_face;
  hb_face_t *hb_face;
  hb_font_t *hb_font;
  double x_scale_inv, y_scale_inv;
  PangoGravity gravity;

  x_scale_inv = y_scale_inv = 1.0;
  key = _pango_fc_font_get_font_key (fc_font);
  if (key)
    {
      const PangoMatrix *matrix = pango_fc_font_key_get_matrix (key);
      pango_matrix_get_font_scale_factors (matrix, &x_scale_inv, &y_scale_inv);
    }
  if (PANGO_GRAVITY_IS_IMPROPER (gravity))
  {
    x_scale_inv = -x_scale_inv;
    y_scale_inv = -y_scale_inv;
  }

  gravity = pango_fc_font_key_get_gravity (key);

  context.x_scale = 1. / x_scale_inv;
  context.y_scale = 1. / y_scale_inv;
  context.ft_face = ft_face;
  context.fc_font = fc_font;
  context.vertical = PANGO_GRAVITY_IS_VERTICAL (gravity);

  hb_face = pango_fc_font_map_get_hb_face (PANGO_FC_FONT_MAP (fc_font->fontmap), fc_font);
  ft_face = pango_fc_font_map_get_ft_face (PANGO_FC_FONT_MAP (fc_font->fontmap), fc_font);
  hb_font = hb_font_create (hb_face);
  hb_font_set_funcs (hb_font,
                     pango_fc_get_hb_font_funcs (),
                     &context,
                     NULL);
  hb_font_set_scale (hb_font,
                     +(((gint64) ft_face->size->metrics.x_scale * ft_face->units_per_EM) >> 12) * context.x_scale,
                     -(((gint64) ft_face->size->metrics.y_scale * ft_face->units_per_EM) >> 12) * context.y_scale);
  hb_font_set_ppem (hb_font,
                    fc_font->is_hinted ? ft_face->size->metrics.x_ppem : 0,
                    fc_font->is_hinted ? ft_face->size->metrics.y_ppem : 0);
  if (key)
    {
      const char *variations = pango_fc_font_key_get_variations (key);
      if (variations)
        {
          guint n_variations;
          hb_variation_t *hb_variations;

          parse_variations (variations, &hb_variations, &n_variations);
          hb_font_set_variations (hb_font, hb_variations, n_variations);

          g_free (hb_variations);
        }
    }

  return hb_font;
}
