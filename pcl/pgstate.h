/* Copyright (C) 1996 Aladdin Enterprises.  All rights reserved.
   Unauthorized use, copying, and/or distribution prohibited.
 */

/* pgstate.h */
/* Definition of HP-GL/2 portion of PCL5 state */

#ifndef pgstate_INCLUDED
#  define pgstate_INCLUDED

/* HPGL/2 coordinates are internally represented in plotter units
   1/1024" when scaling is off and user units when scaling is in
   effect.  The data structure g.pos maintains the coordinates in the
   hpgl/2 state.  By default the coordinate system sets up the origin
   in the lower left of the page with X increasing along the short
   edge and Y increasing up the long edge.  Note the Y direction is
   opposite PCL's. */

#include "gslparam.h"
#include "gsuid.h"		/* for gxbitmap.h */
#include "gstypes.h"		/* for gxbitmap.h */
#include "gxbitmap.h"
/*#include "gzpath.h"*/		/* for gx_path_s */
#include "gxfixed.h"
#ifndef gs_imager_state_DEFINED
#  define gs_imager_state_DEFINED
typedef struct gs_imager_state_s gs_imager_state;
#endif
#include "gzpath.h"

/* Opaque type for a path */
#ifndef gx_path_DEFINED
#  define gx_path_DEFINED
typedef struct gx_path_s gx_path;
#endif

/* Define a type for HP-GL/2 floating point values. */
typedef double hpgl_real_t;

/* scaling definition */
typedef struct hpgl_scaling_params_s {
  gs_point pmin, pmax, factor;
  float left, bottom;
} hpgl_scaling_params_t;

/* Define a line type (a.k.a. dash pattern). */
typedef struct hpgl_line_type_s {
  int count;
  hpgl_real_t gap[20];
} hpgl_line_type_t;

typedef struct hpgl_path_state_s {
  bool have_first_moveto;  
  gx_path path; 
} hpgl_path_state_t;

/* Define the current rendering mode - character, polygon, or vector.
   This will affect the line attributes chosen see
   hpgl_set_graphics_line_attribute_state.  And defines if we use
   stroke or fill on the path.  */
typedef enum {
	hpgl_rm_vector,
	hpgl_rm_character,
	hpgl_rm_polygon,
	hpgl_rm_vector_fill,
	hpgl_rm_clip_and_fill_polygon, /* for hpgl/2 line type filling */
	hpgl_rm_nop            /* don't do anything with the path. future use */
} hpgl_rendering_mode_t;

/* state of lost mode */
typedef enum {
	hpgl_lost_mode_entered,
	hpgl_lost_mode_cleared
} hpgl_lost_mode_t;

typedef enum {
	hpgl_even_odd_rule,
	hpgl_winding_number_rule
} hpgl_render_fill_type_t;

/* Define the structure for saving the pen state temporarily. */
/* HAS: note don't mix and match save a restores.  perhaps there
   should be a type check field in the structure.  */
typedef struct hpgl_pen_state_s {
	int relative_coords;
	int move_or_draw;
	gs_point pos;
} hpgl_pen_state_t;

/* Define the parameters for GL hatch/cross-hatch fill types. */
typedef struct hpgl_hatch_params_s {
  float spacing;
  float angle;
} hpgl_hatch_params_t;

/*
 * Define the functions for adding points to paths.  Note that the
 * move/draw and absolute/relative alternatives can be tested, set, etc.
 * individually.
 */
#define hpgl_plot_absolute 0
#define hpgl_plot_relative 1
#define hpgl_plot_is_absolute(func) (((func) & hpgl_plot_relative) == 0)
#define hpgl_plot_is_relative(func) (((func) & hpgl_plot_relative) != 0)
#define hpgl_plot_move 0
#define hpgl_plot_draw 2
#define hpgl_plot_is_move(func) (((func) & hpgl_plot_draw) == 0)
#define hpgl_plot_is_draw(func) (((func) & hpgl_plot_draw) != 0)
typedef enum {
  hpgl_plot_move_absolute = hpgl_plot_move | hpgl_plot_absolute,
  hpgl_plot_move_relative = hpgl_plot_move | hpgl_plot_relative,
  hpgl_plot_draw_absolute = hpgl_plot_draw | hpgl_plot_absolute,
  hpgl_plot_draw_relative = hpgl_plot_draw | hpgl_plot_relative
} hpgl_plot_function_t;
#define hpgl_plot_function_procedures\
  gs_moveto, gs_rmoveto, gs_lineto, gs_rlineto

typedef struct pcl_hpgl_state_s {
		/* Chapter 17 lost mode (pgmisc.c) */
  
	/* According to PCLTRM IN, PG, RP and PA with args in range clears
	   lost mode.  Note that all these commands have PA with valid args
	   as a side effect so only PA needs to clear lost mode.  */

	hpgl_lost_mode_t lost_mode; 

		/* Chapter 18 (pgframe.c) */

	struct pf_ {
	  coord_point size;
	  coord_point anchor_point;
	} picture_frame;

#define picture_frame_width picture_frame.size.x
#define picture_frame_height picture_frame.size.y

	coord_point plot_size;

#define plot_width plot_size.x
#define plot_height plot_size.y

		/* Chapter 19 (pgconfig.c) */

  	enum {
	  hpgl_scaling_none = -1,
	  hpgl_scaling_anisotropic = 0,
	  hpgl_scaling_isotropic = 1,
	  hpgl_scaling_point_factor = 2
	} scaling_type;
	hpgl_scaling_params_t scaling_params;
	gs_rect window;          /* clipping window (IW) */
	int rotation;
	gs_point P1, P2;	/* in plotter units */

		/* Chapter 20 (pgvector.c) */
  
	int move_or_draw;	/* hpgl_plot_move/draw */
	int relative_coords;	/* hpgl_plot_absolute/relative */
	bool have_first_moveto;  
	bool saved_have_first_moveto; /* used with gsave and grestore */
        gs_point pos;
        /* used to track the line drawing state in hpgl */
        gs_point first_point;
	hpgl_rendering_mode_t current_render_mode; /* HAS revisit */

		/* Chapter 21 (pgpoly.c) */

	hpgl_path_state_t polygon_buffer;
	bool polygon_mode;

		/* Chapter 22 (pglfill.c) */

	struct lp_ {
	  struct ltl_ {
	    int type;
	    float pattern_length;
	    bool pattern_length_relative;
	    bool is_solid;
	  } current, saved;	/* enable saving for LT99 */
	  int cap;
	  int join;
	} line;
	float miter_limit;
	struct pen_ {
	  /**** FOLLOWING SHOULD BE [number_of_pens] ****/
	  float width[2];	/* millimeters or % */
	  bool width_relative;
	  int selected;		/* currently selected pen # */
	} pen;
	byte symbol_mode;	/* 0 if not in symbol mode */
	struct ft_ {
	  enum {
	    hpgl_fill_solid = 1,
	    hpgl_fill_solid2 = 2,
	    hpgl_fill_hatch = 3,
	    hpgl_fill_crosshatch = 4,
	    hpgl_fill_shaded = 10,
	    hpgl_fill_hpgl_user_defined = 11,
	    hpgl_fill_pcl_crosshatch = 21,
	    hpgl_fill_pcl_user_defined = 22
	  } type;
	  /*
	   * Because each fill type remembers its previous parameter values,
	   * we must use a structure rather than a union here.
	   */
	  struct fp_ {
	    hpgl_hatch_params_t hatch;
	    hpgl_hatch_params_t crosshatch;
	    float shading;
	    int pattern_index;
	    int pattern_type;
	    uint pattern_id;
	  } param;
	} fill;
	hpgl_render_fill_type_t fill_type;
	hpgl_line_type_t fixed_line_type[8];
	hpgl_line_type_t adaptive_line_type[8];
	gs_point anchor_corner;
	gx_bitmap fill_pattern[8];
	bool source_transparent;
	struct scr_ {
	  enum {
	    hpgl_screen_none = 0,
	    hpgl_screen_shaded_fill = 1,
	    hpgl_screen_hpgl_user_defined = 2,
	    hpgl_screen_crosshatch = 21,
	    hpgl_screen_pcl_user_defined = 22
	  } type;
	  union su_ {
	    float shading;
	    struct { int pattern_index; bool use_current_pen; } user_defined;
	    int pattern_type;
	    uint pattern_id;
	  } param;
	} screen;
	    /* Temporary while downloading raster fill pattern */
	struct rf_ {
	  int index, width, height;
	  uint raster;
	  byte *data;
	} raster_fill;

		/* Chapter 23 (pgchar.c, pglabel.c) */

	pcl_font_selection_t font_selection[2];
	int font_selected;	/* 0 or 1 */
	pl_font_t *font;	/* 0 means recompute from params */
	pl_symbol_map_t *map;	/* map for current font */
	pl_font_t stick_font[2];	/* stick font, if currently selected */
	struct ch_ {
	  gs_point direction;
	  bool direction_relative;
	  int text_path;
	  bool reverse_line_feed;
	  gs_point extra_space;
	  gs_point size;
	  bool size_relative;
	  bool size_set;
	  hpgl_real_t slant;
	  enum {
	    hpgl_char_solid_edge = 0,
	    hpgl_char_edge = 1,
	    hpgl_char_fill = 2,
	    hpgl_char_fill_edge = 3
	  } fill_mode;
	  int edge_pen;		/* 0 = no edge */
	} character;
	struct lb_ {
	  int origin;
	  uint terminator;
	  bool print_terminator;
	  /* NOTE: HAS These pointer could be local to hpgl_LB but the
             parser can recall hpgl_LB for more data.  So for
             simplicity we make them part of the global state */
#define hpgl_char_count 128     /* initial buffer size */
	  byte *buffer;         /* start of line buffer pointer */
          unsigned int buffer_size; /* size of the current buffer */
	  unsigned int char_count;  /* count of chars in the buffer */
	} label;
	bool transparent_data;
	uint font_id[2];
	bool bitmap_fonts_allowed;
	gs_point carriage_return_pos;

		/* Chapter C7 (pgcolor.c) */

	/**** FOLLOWING SHOULD BE [number_of_pens] ****/
	struct { hpgl_real_t rgb[3]; } pen_color[2];
	uint number_of_pens;
	struct { hpgl_real_t cmin, cmax; } color_range[3];

} pcl_hpgl_state_t;

#define hpgl_pen_relative (1)
#define hpgl_pen_down (1<<1)
#define hpgl_pen_pos (1<<2)
#define hpgl_pen_all (hpgl_pen_relative | hpgl_pen_down | hpgl_pen_pos)

/* HAS we may wish to make these functions to save on code space */
#define hpgl_save_pen_state(pgls, save, save_flags)\
do {\
  if ( (save_flags) & hpgl_pen_relative )\
    ((save)->relative_coords = (pgls)->g.relative_coords);\
  if ( (save_flags) & hpgl_pen_down )\
    ((save)->move_or_draw = (pgls)->g.move_or_draw);\
  if ( (save_flags) & hpgl_pen_pos )\
    ((save)->pos = (pgls)->g.pos);\
} while (0)

#define hpgl_restore_pen_state(pgls, save, restore_flags)\
do {\
  if ( (restore_flags) & hpgl_pen_relative )\
    ((pgls)->g.relative_coords = (save)->relative_coords);\
  if ( (restore_flags) & hpgl_pen_down )\
    ((pgls)->g.move_or_draw = (save)->move_or_draw);\
  if ( (restore_flags) & hpgl_pen_pos )\
    ((pgls)->g.pos = (save)->pos);\
} while (0)

#endif				/* pgstate_INCLUDED */

