/* setBfree - LV2 GUI
 *
 * Copyright 2011-2012 David Robillard <d@drobilla.net>
 * Copyright (C) 2013 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#define _GNU_SOURCE
#define GL_GLEXT_PROTOTYPES

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <math.h>
#include <pthread.h>

#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>

#include "lv2/lv2plug.in/ns/extensions/ui/ui.h"
#include "pugl/pugl.h"

#ifdef __APPLE__
#include "OpenGL/glu.h"
#else
#include <GL/glu.h>
#endif

#include <FTGL/ftgl.h>
#ifndef FONTFILE
#define FONTFILE "/usr/share/fonts/truetype/ttf-bitstream-vera/VeraBd.ttf"
#endif

#include "uris.h"
#include "ui_model.h"

/* ui-model scale -- on screen we use [-1..+1] orthogonal projection */
#define SCALE (0.04f)


#define SIGNUM(a) (a < 0 ? -1 : 1)
#define CTRLWIDTH2(ctrl) (SCALE * (ctrl).w / 2.0)
#define CTRLHEIGHT2(ctrl) (SCALE * (ctrl).h / 2.0)

#define MOUSEOVER(ctrl, mousex, mousey) \
  (   (mousex) >= (ctrl).x * SCALE - CTRLWIDTH2(ctrl) \
   && (mousex) <= (ctrl).x * SCALE + CTRLWIDTH2(ctrl) \
   && (mousey) >= (ctrl).y * SCALE - CTRLHEIGHT2(ctrl) \
   && (mousey) <= (ctrl).y * SCALE + CTRLHEIGHT2(ctrl) )


/* total number of interactive objects */
#define TOTAL_OBJ (33)

/* names from src/midi.c -  mapped to object IDs */
static const char *obj_control[] = {
  "upper.drawbar16", // 0
  "upper.drawbar513",
  "upper.drawbar8",
  "upper.drawbar4",
  "upper.drawbar223",
  "upper.drawbar2",
  "upper.drawbar135",
  "upper.drawbar113",
  "upper.drawbar1", // 8

  "lower.drawbar16",
  "lower.drawbar513",
  "lower.drawbar8",
  "lower.drawbar4",
  "lower.drawbar223",
  "lower.drawbar2",
  "lower.drawbar135",
  "lower.drawbar113",
  "lower.drawbar1", // 17

  "pedal.drawbar16",
  "pedal.drawbar8",

  "percussion.enable",// 20
  "percussion.volume",
  "percussion.decay",
  "percussion.harmonic",
  "vibrato.routing",  // 24  SPECIAL -- lower
  "vibrato.routing",  // 25  SPECIAL -- upper
  "overdrive.enable", // 26
  "overdrive.character",
  "vibrato.knob", // 28
  "swellpedal1",
  "reverb.mix", // 30
  "rotary.speed-select", // SPECIAL leslie horn  // rotary.speed-select 2^3
  "rotary.speed-select"  // SPECIAL leslie baffle
};


typedef struct {
  int type; // type ID from ui_model.h
  float min, max, cur;  // value range and current value
  float x,y; // matrix position
  float w,h; // bounding box
  int texID; // texture ID
  char midinfo[1024];
} b3widget;

typedef struct {
  LV2_Atom_Forge forge;

  LV2_URID_Map* map;
  setBfreeURIs  uris;

  LV2UI_Write_Function write;
  LV2UI_Controller     controller;

  PuglView*            view;
  int                  width;
  int                  height;
  int                  initialized;

#ifdef OLD_SUIL
  pthread_t            thread;
  int                  exit;
#endif

  /* OpenGL */
  GLuint * vbo;
  GLuint * vinx;
  GLuint texID[15]; // textures
  GLdouble matrix[16]; // used for mouse mapping
  double rot[3], off[3], scale; // global projection

  int displaymode;
  int pgm_sel;
  int show_mm;
  int uiccbind;

  int textentry_active;
  char textentry_text[1024];
  char textentry_title[128];

  /* interactive control objexts */
  b3widget ctrls[TOTAL_OBJ];

  /* mouse drag status */
  int dndid;
  float dndval;
  float dndx, dndy;

  FTGLfont *font_big;
  FTGLfont *font_small;

  char *popupmsg;

  char midipgm[128][32];
  char mididsc[128][256];

  char *curdir;
  char **dirlist;
  int dirlistlen;
  int dir_sel;
  int dir_scroll;
  int dir_scrollgrab;

} B3ui;

/******************************************************************************
 * file-name helper function
 */
static void free_dirlist(B3ui* ui) {
  int i;
  if (!ui->dirlist) return;
  for (i=0; i < ui->dirlistlen; ++i) {
    free(ui->dirlist[i]);
  }
  free(ui->dirlist);
  ui->dirlistlen = 0;
  ui->dirlist = NULL;
}

static char * absfilepath(const char *dir, const char *file) {
  char *fn = malloc((strlen(dir) + strlen(file) + 2)*sizeof(char));
  strcpy(fn, dir);
  strcat(fn, "/");
  strcat(fn, file);
  char * rfn = realpath(fn, NULL);
  free(fn);
  return rfn;
}

static int cmpstringp(const void *p1, const void *p2) {
  return strcmp(* (char * const *) p1, * (char * const *) p2);
}

static int dirlist(PuglView* view, const char *dir) {
  B3ui* ui = (B3ui*)puglGetHandle(view);
  DIR  *D;
  struct dirent *dd;

  free_dirlist(ui);

  if (!(D = opendir (dir)))  {
    return -1;
  }

  while ((dd = readdir (D))) {
    struct stat fs;
    char * rfn = absfilepath(dir, dd->d_name);
    if (!rfn) continue;
    if(stat(rfn, &fs)) {
      printf("stat failed: %s\n", rfn);
      free(rfn);
      continue;
    }

    if (S_ISREG(fs.st_mode)) {
      int fnl = strlen(rfn);
      if (fnl <= 4) continue;
      if (strcmp(&rfn[fnl-4], ".pgm") && strcmp(&rfn[fnl-4], ".cfg")) {
	printf("EXT SKIPPED: %s\n", &rfn[fnl-4]);
	free(rfn);
	continue;
      }
    }
    free(rfn);

#if 0
    if (dd->d_name[0] == '.') continue;
#elif 1
    int delen = strlen(dd->d_name);
    if (delen == 1 && dd->d_name[0] == '.') continue; // '.'
    //if (delen == 2 && dd->d_name[0] == '.' && dd->d_name[1] == '.') continue; // '..'
#endif

    ui->dirlist = realloc(ui->dirlist, (ui->dirlistlen+1) * sizeof(char*));
#if 0
    ui->dirlist[ui->dirlistlen] = strdup(dd->d_name);
#else
    ui->dirlist[ui->dirlistlen] = malloc(1024*sizeof(char));
    strncpy(ui->dirlist[ui->dirlistlen], dd->d_name, 1024);
#endif
    ui->dirlistlen++;
  }
  qsort(ui->dirlist, ui->dirlistlen, sizeof(ui->dirlist[0]), cmpstringp);
  return 0;
}

/******************************************************************************
 * Value mapping, MIDI <> internal min/max <> mouse
 */

static void vmap_midi_to_val(PuglView* view, int elem, int mval) {
  B3ui* ui = (B3ui*)puglGetHandle(view);
  ui->ctrls[elem].cur = ui->ctrls[elem].min + rint((ui->ctrls[elem].max - ui->ctrls[elem].min) * mval / 127.0);
}

static unsigned char vmap_val_to_midi(PuglView* view, int elem) {
  B3ui* ui = (B3ui*)puglGetHandle(view);
  const int v = rint( rint(ui->ctrls[elem].cur - ui->ctrls[elem].min) * 127.0 / (ui->ctrls[elem].max - ui->ctrls[elem].min));
  return (v&0x7f);
}

/* call lv2 plugin if value has changed */
#define OBJ_BUF_SIZE 256
static void notifyPlugin(PuglView* view, int elem) {
  B3ui* ui = (B3ui*)puglGetHandle(view);
  uint8_t obj_buf[OBJ_BUF_SIZE];
  int32_t val;

  /* special cases */
  if (elem == 24 || elem == 25) {
    // two in one
    val = ((ui->ctrls[24].cur ? 1 : 0) | (ui->ctrls[25].cur ? 2 : 0) ) << 5;
  } else if (elem == 31 || elem == 32) {
    // map: tremolo/fast 2 << off:1 >> chorale/slow:0  ->  off:0, slow:1, fast:2
    int hr = rint(ui->ctrls[32].cur);
    int bf = rint(ui->ctrls[31].cur);
    if (hr != 2) hr = (hr == 1) ? 0 : 1;
    if (bf != 2) bf = (bf == 1) ? 0 : 1;
    val = bf * 15 + hr * 45;
  } else {
    // default MIDI-CC range 0..127
    val = vmap_val_to_midi(view, elem);
  }

  lv2_atom_forge_set_buffer(&ui->forge, obj_buf, OBJ_BUF_SIZE);
  LV2_Atom* msg = forge_kvcontrolmessage(&ui->forge, &ui->uris, obj_control[elem], val);
  if (msg)
    ui->write(ui->controller, 0, lv2_atom_total_size(msg), ui->uris.atom_eventTransfer, msg);
}

static void forge_message_kv(B3ui* ui, LV2_URID uri, int key, const char *val) {
  uint8_t obj_buf[256];
  if (!val || strlen(val) > 32) { return; }

  lv2_atom_forge_set_buffer(&ui->forge, obj_buf, 64);

  LV2_Atom_Forge_Frame set_frame;
  LV2_Atom* msg = (LV2_Atom*)lv2_atom_forge_blank(&ui->forge, &set_frame, 1, uri);
  lv2_atom_forge_property_head(&ui->forge, ui->uris.sb3_cckey, 0);
  lv2_atom_forge_int(&ui->forge, key);
  lv2_atom_forge_property_head(&ui->forge, ui->uris.sb3_ccval, 0);
  lv2_atom_forge_string(&ui->forge, val, strlen(val));
  lv2_atom_forge_pop(&ui->forge, &set_frame);
  ui->write(ui->controller, 0, lv2_atom_total_size(msg), ui->uris.atom_eventTransfer, msg);
}

static void forge_message_str(B3ui* ui, LV2_URID uri, const char *key) {
  uint8_t obj_buf[1024];
  lv2_atom_forge_set_buffer(&ui->forge, obj_buf, 64);

  LV2_Atom_Forge_Frame set_frame;
  LV2_Atom* msg = (LV2_Atom*)lv2_atom_forge_blank(&ui->forge, &set_frame, 1, uri);
  if (key) {
    lv2_atom_forge_property_head(&ui->forge, ui->uris.sb3_cckey, 0);
    lv2_atom_forge_string(&ui->forge, key, strlen(key));
  }
  lv2_atom_forge_pop(&ui->forge, &set_frame);
  ui->write(ui->controller, 0, lv2_atom_total_size(msg), ui->uris.atom_eventTransfer, msg);
}

static void forge_message_int(B3ui* ui, LV2_URID uri, const int val) {
  uint8_t obj_buf[64];
  lv2_atom_forge_set_buffer(&ui->forge, obj_buf, 64);

  LV2_Atom_Forge_Frame set_frame;
  LV2_Atom* msg = (LV2_Atom*)lv2_atom_forge_blank(&ui->forge, &set_frame, 1, uri);
  lv2_atom_forge_property_head(&ui->forge, ui->uris.sb3_cckey, 0);
  lv2_atom_forge_int(&ui->forge, val);
  lv2_atom_forge_pop(&ui->forge, &set_frame);
  ui->write(ui->controller, 0, lv2_atom_total_size(msg), ui->uris.atom_eventTransfer, msg);
}

/* called from port_event -- plugin tells GUI a new value */
static void processCCevent(B3ui* ui, const char *k, int v) {
  int i;
  /* special cases */
  if (!strcmp("vibrato.routing", k)) {
    ui->ctrls[24].cur = ((v>>5) & 1 ) ? 1 : 0;
    ui->ctrls[25].cur = ((v>>5) & 2 ) ? 1 : 0;
    puglPostRedisplay(ui->view);
    return;
  } else
  if (!strcmp("rotary.speed-select", k)) {
    // see setRevOption() -- value 0..8
    // map: off:0, slow:1, fast:2  ->  tremolo/fast 2 << off:1 >> chorale/slow:0
    int hr = (v / 45) % 3; // horn 0:off, 1:chorale  2:tremolo
    int bf = (v / 15) % 3; // drum 0:off, 1:chorale  2:tremolo
    if (hr != 2) hr = (hr == 1) ? 0 : 1;
    if (bf != 2) bf = (bf == 1) ? 0 : 1;
    ui->ctrls[31].cur = hr; // horn 0:chorale, 1:off, 2:tremolo
    ui->ctrls[32].cur = bf; // drum 0:chorale, 1:off, 2:tremolo
    puglPostRedisplay(ui->view);
    return;
  }

  for (i = 0; i < TOTAL_OBJ; ++i) {
    if (!strcmp(obj_control[i], k)) {
      /* override drags/modifications of current object */
      if (ui->dndid == i) {
	ui->dndid = -1;
      }
      vmap_midi_to_val(ui->view, i, v);
      puglPostRedisplay(ui->view);
      break;
    }
  }
}


/* process mouse motion, update value */
static void processMotion(PuglView* view, int elem, float dx, float dy) {
  B3ui* ui = (B3ui*)puglGetHandle(view);
  if (elem < 0 || elem >= TOTAL_OBJ) return;

  const float dist = -2.0 * (ui->ctrls[elem].type == OBJ_LEVER ? 2.5 * dx : dy);
  const unsigned char oldval = vmap_val_to_midi(view, elem);

  switch (ui->ctrls[elem].type) {
    case OBJ_DIAL:
      ui->ctrls[elem].cur = ui->dndval + dist * (ui->ctrls[elem].max - ui->ctrls[elem].min);
      if (ui->ctrls[elem].max == 0) {
	if (ui->ctrls[elem].cur > ui->ctrls[elem].max || ui->ctrls[elem].cur < ui->ctrls[elem].min) {
	  const float r = (ui->ctrls[elem].max - ui->ctrls[elem].min);
	  ui->ctrls[elem].cur -= ceil(ui->ctrls[elem].cur / r) * r;
	}
      } else {
	if (ui->ctrls[elem].cur > ui->ctrls[elem].max) ui->ctrls[elem].cur = ui->ctrls[elem].max;
	if (ui->ctrls[elem].cur < ui->ctrls[elem].min) ui->ctrls[elem].cur = ui->ctrls[elem].min;
      }
      break;
    case OBJ_LEVER:
    case OBJ_DRAWBAR:
      ui->ctrls[elem].cur = ui->dndval + dist * (ui->ctrls[elem].max - ui->ctrls[elem].min);
      if (ui->ctrls[elem].cur > ui->ctrls[elem].max) ui->ctrls[elem].cur = ui->ctrls[elem].max;
      if (ui->ctrls[elem].cur < ui->ctrls[elem].min) ui->ctrls[elem].cur = ui->ctrls[elem].min;
      break;
    default:
      break;
  }

  if (vmap_val_to_midi(view, elem) != oldval) {
    puglPostRedisplay(view);
    notifyPlugin(view, elem);
  }
}


/******************************************************************************
 * 3D projection
 */

/* invert projection matrix -- code from GLU */
static bool invertMatrix(const double m[16], double invOut[16]) {
  double inv[16], det;
  int i;

  inv[0] = m[5]  * m[10] * m[15] -
	   m[5]  * m[11] * m[14] -
	   m[9]  * m[6]  * m[15] +
	   m[9]  * m[7]  * m[14] +
	   m[13] * m[6]  * m[11] -
	   m[13] * m[7]  * m[10];

  inv[4] = -m[4]  * m[10] * m[15] +
	    m[4]  * m[11] * m[14] +
	    m[8]  * m[6]  * m[15] -
	    m[8]  * m[7]  * m[14] -
	    m[12] * m[6]  * m[11] +
	    m[12] * m[7]  * m[10];

  inv[8] = m[4]  * m[9]  * m[15] -
	   m[4]  * m[11] * m[13] -
	   m[8]  * m[5]  * m[15] +
	   m[8]  * m[7]  * m[13] +
	   m[12] * m[5]  * m[11] -
	   m[12] * m[7]  * m[9];

  inv[12] = -m[4]  * m[9]  * m[14] +
	     m[4]  * m[10] * m[13] +
	     m[8]  * m[5]  * m[14] -
	     m[8]  * m[6]  * m[13] -
	     m[12] * m[5]  * m[10] +
	     m[12] * m[6]  * m[9];

  inv[1] = -m[1]  * m[10] * m[15] +
	    m[1]  * m[11] * m[14] +
	    m[9]  * m[2]  * m[15] -
	    m[9]  * m[3]  * m[14] -
	    m[13] * m[2]  * m[11] +
	    m[13] * m[3]  * m[10];

  inv[5] = m[0]  * m[10] * m[15] -
	   m[0]  * m[11] * m[14] -
	   m[8]  * m[2]  * m[15] +
	   m[8]  * m[3]  * m[14] +
	   m[12] * m[2]  * m[11] -
	   m[12] * m[3]  * m[10];

  inv[9] = -m[0]  * m[9]  * m[15] +
	    m[0]  * m[11] * m[13] +
	    m[8]  * m[1]  * m[15] -
	    m[8]  * m[3]  * m[13] -
	    m[12] * m[1]  * m[11] +
	    m[12] * m[3]  * m[9];

  inv[13] = m[0]  * m[9]  * m[14] -
	    m[0]  * m[10] * m[13] -
	    m[8]  * m[1]  * m[14] +
	    m[8]  * m[2]  * m[13] +
	    m[12] * m[1]  * m[10] -
	    m[12] * m[2]  * m[9];

  inv[2] = m[1]  * m[6] * m[15] -
	   m[1]  * m[7] * m[14] -
	   m[5]  * m[2] * m[15] +
	   m[5]  * m[3] * m[14] +
	   m[13] * m[2] * m[7] -
	   m[13] * m[3] * m[6];

  inv[6] = -m[0]  * m[6] * m[15] +
	    m[0]  * m[7] * m[14] +
	    m[4]  * m[2] * m[15] -
	    m[4]  * m[3] * m[14] -
	    m[12] * m[2] * m[7] +
	    m[12] * m[3] * m[6];

  inv[10] = m[0]  * m[5] * m[15] -
	    m[0]  * m[7] * m[13] -
	    m[4]  * m[1] * m[15] +
	    m[4]  * m[3] * m[13] +
	    m[12] * m[1] * m[7] -
	    m[12] * m[3] * m[5];

  inv[14] = -m[0]  * m[5] * m[14] +
	     m[0]  * m[6] * m[13] +
	     m[4]  * m[1] * m[14] -
	     m[4]  * m[2] * m[13] -
	     m[12] * m[1] * m[6] +
	     m[12] * m[2] * m[5];

  inv[3] = -m[1] * m[6] * m[11] +
	    m[1] * m[7] * m[10] +
	    m[5] * m[2] * m[11] -
	    m[5] * m[3] * m[10] -
	    m[9] * m[2] * m[7] +
	    m[9] * m[3] * m[6];

  inv[7] = m[0] * m[6] * m[11] -
	   m[0] * m[7] * m[10] -
	   m[4] * m[2] * m[11] +
	   m[4] * m[3] * m[10] +
	   m[8] * m[2] * m[7] -
	   m[8] * m[3] * m[6];

  inv[11] = -m[0] * m[5] * m[11] +
	     m[0] * m[7] * m[9] +
	     m[4] * m[1] * m[11] -
	     m[4] * m[3] * m[9] -
	     m[8] * m[1] * m[7] +
	     m[8] * m[3] * m[5];

  inv[15] = m[0] * m[5] * m[10] -
	    m[0] * m[6] * m[9] -
	    m[4] * m[1] * m[10] +
	    m[4] * m[2] * m[9] +
	    m[8] * m[1] * m[6] -
	    m[8] * m[2] * m[5];

  det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];

  if (det == 0) return false;

  det = 1.0 / det;

  for (i = 0; i < 16; i++)
    invOut[i] = inv[i] * det;

  return true;
}

#ifdef DEBUG_ROTATION_MATRIX
static void print4x4(GLdouble *m) {
  fprintf(stderr,
      "%+0.3lf %+0.3lf %+0.3lf %+0.3lf\n"
      "%+0.3lf %+0.3lf %+0.3lf %+0.3lf\n"
      "%+0.3lf %+0.3lf %+0.3lf %+0.3lf\n"
      "%+0.3lf %+0.3lf %+0.3lf %+0.3lf;\n\n"
      , m[0] , m[1] , m[2] , m[3]
      , m[4] , m[5] , m[6] , m[7]
      , m[8] , m[9] , m[10] , m[11]
      , m[12] , m[13] , m[14] , m[15]
      );
}
#endif

/* apply reverse projection to mouse-pointer, project Z-axis to screen. */
static void project_mouse(PuglView* view, int mx, int my, float *x, float *y) {
  B3ui* ui = (B3ui*)puglGetHandle(view);
  const double fx =  2.0 * (float)mx / ui->width  - 1.0;
  const double fy = -2.0 * (float)my / ui->height + 1.0;
  const double fz = -(fx * ui->matrix[2] + fy * ui->matrix[6]) / ui->matrix[10];

  *x = fx * ui->matrix[0] + fy * ui->matrix[4] + fz * ui->matrix[8] + ui->matrix[12];
  *y = fx * ui->matrix[1] + fy * ui->matrix[5] + fz * ui->matrix[9] + ui->matrix[13];
}


/******************************************************************************
 * 3D model loading
 * see http://ksolek.fm.interia.pl/Blender/
 */

static void initMesh(PuglView* view) {
  B3ui* ui = (B3ui*)puglGetHandle(view);
  int i;

  glGenBuffers(OBJECTS_COUNT, ui->vbo);

  for (i = 0; i < OBJECTS_COUNT; i++) {
    glBindBuffer(GL_ARRAY_BUFFER, ui->vbo[i]);
    glBufferData(GL_ARRAY_BUFFER, sizeof (struct vertex_struct) * vertex_count[i], &vertices[vertex_offset_table[i]], GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
  }

  glGenBuffers(OBJECTS_COUNT, ui->vinx);
  for (i = 0; i < OBJECTS_COUNT; i++) {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ui->vinx[i]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof (indexes[0]) * faces_count[i] * 3, &indexes[indices_offset_table[i]], GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  }
}

#define BUFFER_OFFSET(x)((char *)NULL+(x))

static void drawMesh(PuglView* view, unsigned int index, int apply_transformations) {
  B3ui* ui = (B3ui*)puglGetHandle(view);

  if (apply_transformations) {
    glPushMatrix();
    glMultMatrixf(transformations[index]);
  }

  glBindBuffer(GL_ARRAY_BUFFER, ui->vbo[index]);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ui->vinx[index]);

  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(3, GL_FLOAT, sizeof (struct vertex_struct), BUFFER_OFFSET(0));

  glEnableClientState(GL_NORMAL_ARRAY);
  glNormalPointer(GL_FLOAT, sizeof (struct vertex_struct), BUFFER_OFFSET(3 * sizeof (float)));

  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  glTexCoordPointer(2, GL_FLOAT, sizeof (struct vertex_struct), BUFFER_OFFSET(6 * sizeof (float)));

  glDrawElements(GL_TRIANGLES, faces_count[index] * 3, INX_TYPE, BUFFER_OFFSET(0));

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

  glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_NORMAL_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);

  if (apply_transformations) {
    glPopMatrix();
  }
}

/******************************************************************************
 * OpenGL textures
 */

#include "wood.c"
#include "dial.c"
#include "drawbar.c"

#include "btn_vibl.c"
#include "btn_vibu.c"
#include "btn_perc.c"
#include "btn_perc_decay.c"
#include "btn_perc_harmonic.c"
#include "btn_perc_volume.c"
#include "btn_overdrive.c"

#include "bg_right_ctrl.c"
#include "bg_left_ctrl.c"
#include "bg_leslie_drum.c"
#include "bg_leslie_horn.c"

#include "help_screen_image.c"

#define CIMAGE(ID, VARNAME) \
  glGenTextures(1, &ui->texID[ID]); \
  glBindTexture(GL_TEXTURE_2D, ui->texID[ID]); \
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER); \
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER); \
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_BORDER); \
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR); \
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); \
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, VARNAME.width, VARNAME.height, 0, \
      (VARNAME.bytes_per_pixel == 3 ? GL_RGB : GL_RGBA), \
      GL_UNSIGNED_BYTE, VARNAME.pixel_data); \
  if (atihack) { \
    glEnable(GL_TEXTURE_2D); \
    glGenerateMipmapEXT(GL_TEXTURE_2D); \
    glDisable(GL_TEXTURE_2D); \
  } else { \
    glGenerateMipmapEXT(GL_TEXTURE_2D); \
  }


static void initTextures(PuglView* view) {
  B3ui* ui = (B3ui*)puglGetHandle(view);
  const int atihack = 1; // TODO detect card

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  glGenTextures(1, &ui->texID[0]);
  glBindTexture(GL_TEXTURE_2D, ui->texID[0]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, wood_image.width, wood_image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, wood_image.pixel_data);
  if (atihack) {
    glEnable(GL_TEXTURE_2D);
    glGenerateMipmapEXT(GL_TEXTURE_2D);
    glDisable(GL_TEXTURE_2D);
  } else {
    glGenerateMipmapEXT(GL_TEXTURE_2D);
  }

  CIMAGE(1, drawbar_image);
  CIMAGE(2, dial_image);

  CIMAGE(3, btn_vibl_image);
  CIMAGE(4, btn_vibu_image);
  CIMAGE(5, btn_overdrive_image);

  CIMAGE(6, btn_perc_image);
  CIMAGE(7, btn_perc_vol_image);
  CIMAGE(8, btn_perc_decay_image);
  CIMAGE(9, btn_perc_harm_image);

  CIMAGE(10, bg_right_ctrl_image);
  CIMAGE(11, bg_left_ctrl_image);
  CIMAGE(12, bg_leslie_drum_image);
  CIMAGE(13, bg_leslie_horn_image);
  CIMAGE(14, help_screen_image);
}


/******************************************************************************
 * OpenGL settings
 */

static void setupOpenGL() {
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glFrontFace(GL_CCW);
  glEnable(GL_CULL_FACE);
  glEnable(GL_DITHER);
  glEnable(GL_MULTISAMPLE);
  glEnable(GL_NORMALIZE);
  glEnable(GL_POLYGON_SMOOTH);
  glEnable (GL_LINE_SMOOTH);
  glShadeModel(GL_SMOOTH);

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_SRC_ALPHA_SATURATE);

  glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
  glHint(GL_POLYGON_SMOOTH_HINT, GL_NICEST);
  glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
  glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);
  glHint(GL_GENERATE_MIPMAP_HINT, GL_NICEST);
  glHint(GL_FOG_HINT, GL_NICEST);

  //glPolygonMode(GL_FRONT_AND_BACK, GL_LINE); // test & debug
}

static void setupLight() {
  const GLfloat light0_ambient[]  = { 0.2, 0.15, 0.1, 1.0 };
  const GLfloat light0_diffuse[]  = { 1.0, 1.0, 1.0, 1.0 };
  const GLfloat light0_specular[] = { 0.9, 0.9, 1.0, 1.0 };
  const GLfloat light0_position[] = {  3.0,  2.5, -10.0, 0 };
  const GLfloat spot_direction[]  = { -2.5, -2.5,  9.0 };

  glLightfv(GL_LIGHT0, GL_AMBIENT, light0_ambient);
  glLightfv(GL_LIGHT0, GL_DIFFUSE, light0_diffuse);
  glLightfv(GL_LIGHT0, GL_SPECULAR, light0_specular);
  glLightfv(GL_LIGHT0, GL_POSITION, light0_position);
  glLightf(GL_LIGHT0,  GL_SPOT_CUTOFF, 10.0f);
  glLightfv(GL_LIGHT0, GL_SPOT_DIRECTION, spot_direction);
#if 0
  glLightf(GL_LIGHT0,  GL_SPOT_EXPONENT, 120.0);
  glLightf(GL_LIGHT0,  GL_CONSTANT_ATTENUATION, 1.5);
  glLightf(GL_LIGHT0,  GL_LINEAR_ATTENUATION, 0.5);
  glLightf(GL_LIGHT0,  GL_QUADRATIC_ATTENUATION, 0.2);

  const GLfloat global_ambient[]  = { 0.2, 0.2, 0.2, 1.0 };
  glLightModelfv(GL_LIGHT_MODEL_AMBIENT, global_ambient);
  glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
  glEnable(GL_COLOR_MATERIAL);
#endif

  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
}

/******************************************************************************
 * puGL callbacks
 */

static void
onReshape(PuglView* view, int width, int height)
{
  B3ui* ui = (B3ui*)puglGetHandle(view);
  const float invaspect = (float) height / (float) width;

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(-1.0, 1.0, invaspect, -invaspect, 3.0, -3.0);

  if (ui->displaymode || ui->textentry_active || ui->popupmsg) {
    glMatrixMode(GL_MODELVIEW);
    return;
  }

  glRotatef(ui->rot[0], 0, 1, 0);
  glRotatef(ui->rot[1], 1, 0, 0);
  glRotatef(ui->rot[2], 0, 0, 1);
  glScalef(ui->scale, ui->scale, ui->scale);
  glTranslatef(ui->off[0], ui->off[1], ui->off[2]);

  GLdouble matrix[16];
  glGetDoublev(GL_PROJECTION_MATRIX, matrix);
  invertMatrix(matrix, ui->matrix);

#ifdef DEBUG_ROTATION_MATRIX
  print4x4(matrix);
  print4x4(ui->matrix);
#endif

  glViewport(0, 0, width, height);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
}

static void
render_title(PuglView* view, const char *text, float x, float y, float z, int align)
{
  B3ui* ui = (B3ui*)puglGetHandle(view);
  const GLfloat mat_w[] = {1.0, 1.0, 1.0, 1.0};
  float bb[6];

  glPushMatrix();
  glLoadIdentity();

  glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, mat_w);
  glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_BLEND);
  glScalef(0.001,0.001,1.00);
  glRotatef(180, 1, 0, 0);

  ftglGetFontBBox(ui->font_big, text, -1, bb);
#if 0
  printf("%.2f %.2f %.2f  %.2f %.2f %.2f\n",
      bb[0], bb[1], bb[2], bb[3], bb[4], bb[5]);
#endif
  switch(align) {
    case 1: // center + middle
      glTranslatef(
	  (bb[3] - bb[0])/-2.0,
	  (bb[4] - bb[1])/-2.0,
	  0);
      break;
    case 2: // right
      glTranslatef(
	  (bb[3] - bb[0])/-1.0,
	  (bb[4] - bb[1])/-1.0,
	  0);
      break;
    case 3: // left bottom
      break;
    default: // left top
      glTranslatef(
	  0,
	  (bb[4] - bb[1])/-1.0,
	  0);
      break;
  }
  glTranslatef(x * (1000.0*SCALE) , y * (1000.0*SCALE), z);
  ftglRenderFont(ui->font_big, text, FTGL_RENDER_ALL);

  glPopMatrix();
}

static void
render_text(PuglView* view, const char *text, float x, float y, float z, int align)
{
  B3ui* ui = (B3ui*)puglGetHandle(view);
  const GLfloat mat_b[] = {0.0, 0.0, 0.0, 1.0};
  const GLfloat mat_r[] = {0.1, 0.9, 0.15, 1.0};
  float bb[6];

  glPushMatrix();
  glLoadIdentity();

  glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, mat_b);
  glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, mat_b);
  glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, mat_r);

  glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_BLEND);
  glScalef(0.001,0.001,1.00);
  glRotatef(180, 1, 0, 0);
  ftglGetFontBBox(ui->font_small, text, -1, bb);
#if 0
  printf("%.2f %.2f %.2f  %.2f %.2f %.2f\n",
      bb[0], bb[1], bb[2], bb[3], bb[4], bb[5]);
#endif
  switch(align) {
    case 1: // center + middle
      glTranslatef(
	  (bb[3] - bb[0])/-2.0,
	  (bb[4] - bb[1])/-2.0,
	  0);
      break;
    case 2: // right
      glTranslatef(
	  (bb[3] - bb[0])/-1.0,
	  (bb[4] - bb[1])/-1.0,
	  0);
      break;
    case 3: // left bottom
      break;
    default: // left top
      glTranslatef(
	  0,
	  (bb[4] - bb[1])/-1.0,
	  0);
      break;
  }

  glTranslatef(x * (1000.0*SCALE) , -y * (1000.0*SCALE), z * SCALE);
  ftglRenderFont(ui->font_small, text, FTGL_RENDER_ALL);
  glPopMatrix();
}

static void
unity_box(PuglView* view,
    const float x0, const float x1,
    const float y0, const float y1,
    const GLfloat color[4])
{
  B3ui* ui = (B3ui*)puglGetHandle(view);
  const float invaspect = (float) ui->height / (float) ui->width;
  glPushMatrix();
  glLoadIdentity();
  glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, color);
  glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, color);
  glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, color);
  glBegin(GL_QUADS);
  glVertex3f(x0, y0 * invaspect, 0);
  glVertex3f(x0, y1 * invaspect, 0);
  glVertex3f(x1, y1 * invaspect, 0);
  glVertex3f(x1, y0 * invaspect, 0);
  glEnd();
  glPopMatrix();
}

/******************************************************************************
 * openGL text entry
 */

static int txtentry_start(PuglView* view, const char *title, char *defaulttext) {
  B3ui* ui = (B3ui*)puglGetHandle(view);
  if (ui->textentry_active) return -1;

  if (!defaulttext) {
    ui->textentry_text[0] = '\0';
  } else {
    strncpy(ui->textentry_text, defaulttext, 1024);
  }
  sprintf(ui->textentry_title, "%s", title);
  ui->textentry_active = 1;
  onReshape(view, ui->width, ui->height);
  puglPostRedisplay(view);
  return 0;
}

static void txtentry_end(PuglView* view, const char *txt) {
  B3ui* ui = (B3ui*)puglGetHandle(view);
  if (!txt || strlen(txt) ==0) return;
  switch(ui->displaymode) {
    case 3:
      if (ui->pgm_sel >= 0) {
	forge_message_kv(ui, ui->uris.sb3_midisavepgm, ui->pgm_sel, txt);
      }
      ui->displaymode = 0;
      break;
    default:
      printf("unhandled text entry (mode:%d)\n", ui->displaymode);
      ui->displaymode = 0;
      break;
  }
  onReshape(view, ui->width, ui->height);
}

static void txtentry_handle(PuglView* view, uint32_t key) {
  B3ui* ui = (B3ui*)puglGetHandle(view);
  int pos = strlen(ui->textentry_text);
  switch (key) {
    case 8:
      if (pos > 0) pos--;
      break;
    case 27:
      pos = 0;
      ui->textentry_text[pos] = '\0';
      // fall through
    case 10:
    case 13:
      ui->textentry_active = 0;
      onReshape(view, ui->width, ui->height);
      txtentry_end(view, ui->textentry_text);
      break;
    default:
      ui->textentry_text[pos++] = (char) key;
      break;
  }
  if (pos > 1023) pos=1023;
  ui->textentry_text[pos] = '\0';
  puglPostRedisplay(view);
}

static void txtentry_render(PuglView* view) {
  B3ui* ui = (B3ui*)puglGetHandle(view);

  const GLfloat mat_b[] = {0.0, 0.0, 0.0, 1.0};
  const GLfloat mat_w[] = {1.0, 1.0, 1.0, 1.0};
  const GLfloat mat_g[] = {0.1, 0.9, 0.15, 1.0};
  const GLfloat mat_x[] = {0.1, 0.1, 0.15, 1.0};

  unity_box(view, -1.0, 1.0, 0, .24, mat_x);

  glPushMatrix();
  glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, mat_b);
  glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, mat_b);
  glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, mat_g);
  glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_BLEND);

  glLoadIdentity();
  glScalef(0.001,0.001,1.00);
  glRotatef(180, 1, 0, 0);
  float bb[6];
  ftglGetFontBBox(ui->font_big, ui->textentry_text, -1, bb);

  glTranslatef((bb[3] - bb[0])/-2.0, -1.5 * (1000.0*SCALE), 0);
  ftglRenderFont(ui->font_big, ui->textentry_text, FTGL_RENDER_ALL);

  glTranslatef((bb[3] - bb[0]), 0, 0);
  glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, mat_w);
  ftglRenderFont(ui->font_big, "|", FTGL_RENDER_ALL);

  glLoadIdentity();
  glScalef(0.001,0.001,1.00);
  glRotatef(180, 1, 0, 0);
  ftglGetFontBBox(ui->font_big, ui->textentry_title, -1, bb);
  glTranslatef((bb[3] - bb[0])/-2.0, (bb[4] - bb[1])/-2.0, 0);
  glTranslatef(0, 4.5 * (1000.0*SCALE), 0);
  ftglRenderFont(ui->font_big, ui->textentry_title, FTGL_RENDER_ALL);

  glPopMatrix();

}

static void show_message(PuglView* view, const char *msg) {
  B3ui* ui = (B3ui*)puglGetHandle(view);
  if (ui->popupmsg) free (ui->popupmsg);
  ui->popupmsg = strdup(msg);
  onReshape(view, ui->width, ui->height);
  puglPostRedisplay(view);
}

/**
 * main display fn
 */

static void
onDisplay(PuglView* view)
{
  int i;
  B3ui* ui = (B3ui*)puglGetHandle(view);

  const GLfloat no_mat[] = { 0.0, 0.0, 0.0, 1.0 };
  const GLfloat mat_specular[] = { 1.0, 1.0, 1.0, 1.0 };
  const GLfloat no_shininess[] = { 128.0 };
  const GLfloat high_shininess[] = { 5.0 };

  const GLfloat mat_organ[] = { 0.5, 0.25, 0.1, 1.0 };
  const GLfloat mat_dial[] = { 0.1, 0.1, 0.1, 1.0 };
  const GLfloat mat_lever[] = { 0.3, 0.3, 0.3, 1.0 };
  const GLfloat mat_switch[] = { 1.0, 1.0, 0.94, 1.0 };
  const GLfloat glow_red[] = { 1.0, 0.0, 0.00, 0.3 };
  const GLfloat mat_drawbar_white[] = { 1.0, 1.0, 1.0, 1.0 };
  const GLfloat mat_drawbar_brown[] = { 0.39, 0.25, 0.1, 1.0 };
  const GLfloat mat_drawbar_black[] = { 0.0, 0.0, 0.0, 1.0 };
  const GLfloat mat_w[] = {3.5, 3.5, 3.5, 1.0};

  if (!ui->initialized) {
    /* initialization needs to happen from event context
     * after pugl set glXMakeCurrent() - this /should/ otherwise
     * be done during initialization()
     */
    ui->initialized = 1;
    setupOpenGL();
    initMesh(ui->view);
    setupLight();
    initTextures(ui->view);
    ui->font_big = ftglCreateBufferFont(FONTFILE);
    ftglSetFontFaceSize(ui->font_big, 80, 72);
    ftglSetFontCharMap(ui->font_big, ft_encoding_unicode);
    ui->font_small = ftglCreateBufferFont(FONTFILE);
    ftglSetFontFaceSize(ui->font_small, 20, 72);
    ftglSetFontCharMap(ui->font_small, ft_encoding_unicode);
  }

  if (ui->popupmsg) {
    unity_box(view, -1.0, 1.0, .25, .25, mat_dial);
    render_title(view, ui->popupmsg, 0, 0, 0, 1);
    return;
  }

  if (ui->textentry_active) {
    txtentry_render(view);
    return;
  }

  if (ui->displaymode == 1) {
    /* Help screen */
    const float invaspect = (float) ui->height / (float) ui->width;
    glMaterialfv(GL_FRONT, GL_DIFFUSE, mat_w);
    glMaterialfv(GL_FRONT, GL_AMBIENT, mat_w);
    glLoadIdentity();
    glEnable(GL_TEXTURE_2D);
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
    glBindTexture(GL_TEXTURE_2D, ui->texID[14]);
    glBegin(GL_QUADS);
    glColor3f(1.0, 1.0, 1.0);
    glTexCoord2f (0.0, 0.0); glVertex3f(-1, -invaspect, 0);
    glTexCoord2f (0.0, 1.0); glVertex3f(-1,  invaspect, 0);
    glTexCoord2f (1.0, 1.0); glVertex3f( 1,  invaspect, 0);
    glTexCoord2f (1.0, 0.0); glVertex3f( 1, -invaspect, 0);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    return;
  } else if (ui->displaymode == 2 || ui->displaymode == 3) {
    /* midi program list */
    int i;
    const float invaspect = (float) ui->height / (float) ui->width;
    const float w = 1.0/2.8 * 22.0 * SCALE;
    const float h = 2.0/24.0 * 22.0 * SCALE;

    render_title(view, (ui->displaymode == 2) ? "load" : "save", 16.5, -7.25, 0.0, 3);

    for (i=0; i < 128; i++) {
      char txt[40];
      sprintf(txt, "p%3d: %s", i+1, ui->midipgm[i]);
      float x = -1.1 +  (i/24)/2.7; // 0..5
      float y = -1.0 +  (i%24)/12.0; // 0..23

      x *= 22.0; y *= 22.0;

      const float bx = x * SCALE;
      const float by = y * SCALE;

      GLfloat mat_x[] = {0.1, 0.1, 0.1, 1.0};
      if (i == ui->pgm_sel) {
	if (ui->displaymode == 2) mat_x[2] = .6;
	else mat_x[0] = .6;
      }
      else if (i%2) {
	mat_x[0] = .125;
	mat_x[1] = .125;
	mat_x[2] = .125;
      }
      unity_box(view, bx, bx+w, by, by+h, mat_x);
      y *= invaspect;
      render_text(view, txt, x, y, .1f, 0);
    }
    if (ui->pgm_sel >= 0) {
      char *t0, *t1; int ln = 0;
      t0 = ui->mididsc[ui->pgm_sel];
      //printf("DSC: %s\n", t0);
      while (*t0 && (t1 = strchr(t0, '\n'))) {
	*t1='\0';
	render_text(view, t0, 16.5, ++ln*0.5, .1f, 3);
	*t1='\n';
	t0=t1+1;
      }
    }
    return;
  } else if (ui->displaymode == 4) {
    int i;
    const float invaspect = (float) ui->height / (float) ui->width;
    const float w = 1.0/2.8 * 22.0 * SCALE;
    const float h = 2.0/24.0 * 22.0 * SCALE;

    render_title(view, (ui->displaymode == 4) ? "open" : "save", 16.5, -7.75, 0.0, 3);
    // TODO handle empty dir

    float xscolloff = 0;
    if (ui->dirlistlen > 120) {
      unity_box(view, -.8, 0.8, 0.625, 0.7, mat_drawbar_white);
      int pages = (ui->dirlistlen / 20);
      float ss = 1.6 / (float)pages;
      float sw = 5.0 * ss;
      float sx = ui->dir_scroll * ss - .8;
      unity_box(view, sx, sx+sw, 0.625, 0.7, mat_organ);
      xscolloff = ui->dir_scroll / 2.7;
    }

    for (i=0; i < ui->dirlistlen; i++) {
      char txt[30];
      snprintf(txt, 24, "%s", ui->dirlist[i]);
      txt[24]='\0';
      if (strlen(ui->dirlist[i]) > 24) strcat(txt, "...");

      float x = -1.1 + (i/20)/2.7; // 0..5
      float y = -1.0 + (i%20)/12.0; // 0..19
      x -= xscolloff;

      x *= 22.0; y *= 22.0;

      const float bx = x * SCALE;
      const float by = y * SCALE + .006;

      GLfloat mat_x[] = {0.1, 0.1, 0.1, 1.0};
      if (i == ui->dir_sel) {
	mat_x[2] = .6;
      }
      else if (i%2) {
	mat_x[0] = .125;
	mat_x[1] = .125;
	mat_x[2] = .125;
      }
      unity_box(view, bx, bx+w, by-h, by, mat_x);

      y *= invaspect;
      render_text(view, txt, x, y, .1f, 3);
    }
    return;
  }

  /* main organ */

  /** step 0 - help button **/

  render_title(view, " ?  ", 1.08/SCALE, .22/SCALE, .055, 1);

  /** step 1 - draw background -- fixed objects **/

  glPushMatrix();
  glLoadIdentity();
  glRotatef(180, 1, 0, 0);
  glScalef(SCALE, SCALE, SCALE);

  /* organ - background */
  glMaterialfv(GL_FRONT, GL_AMBIENT, no_mat);
  glMaterialfv(GL_FRONT, GL_DIFFUSE, mat_organ);
  glMaterialfv(GL_FRONT, GL_SPECULAR, no_mat);
  glMaterialfv(GL_FRONT, GL_SHININESS, no_shininess);
  glMaterialfv(GL_FRONT, GL_EMISSION, no_mat);

  glEnable(GL_TEXTURE_2D);
  glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
  glBindTexture(GL_TEXTURE_2D, ui->texID[0]);

  drawMesh(view, OBJ_ORGANBG, 1);

  glDisable(GL_TEXTURE_2D);
  glPopMatrix();

  /* insets */
  glPushMatrix();
  glLoadIdentity();
  glRotatef(180, 1, 0, 0);

  glScalef(SCALE, SCALE, SCALE);
  glTranslatef(22.875, -1.49067, 0);

  glMaterialfv(GL_FRONT, GL_AMBIENT, mat_dial);
  glMaterialfv(GL_FRONT, GL_DIFFUSE, mat_dial);
  glMaterialfv(GL_FRONT, GL_SPECULAR, mat_specular);
  glMaterialfv(GL_FRONT, GL_SHININESS, high_shininess);
  glMaterialfv(GL_FRONT, GL_EMISSION, no_mat);

  /* right ctrl */
  glEnable(GL_TEXTURE_2D);
  glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
  glBindTexture(GL_TEXTURE_2D, ui->texID[10]);
  drawMesh(view, OBJ_INSET, 1);
  glDisable(GL_TEXTURE_2D);

  /* left ctrl */
  glTranslatef(-22.875 * 2.0, 0, 0);
  glEnable(GL_TEXTURE_2D);
  glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
  glBindTexture(GL_TEXTURE_2D, ui->texID[11]);
  drawMesh(view, OBJ_INSET, 1);
  glDisable(GL_TEXTURE_2D);

  /* leslie drum box */
  glTranslatef(-2.8, -5.50485, 0);
  glEnable(GL_TEXTURE_2D);
  glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
  glBindTexture(GL_TEXTURE_2D, ui->texID[12]);
  drawMesh(view, OBJ_GEARBOX, 1);
  glDisable(GL_TEXTURE_2D);

  /* leslie horn box */
  glTranslatef(5.6, 0, 0);
  glEnable(GL_TEXTURE_2D);
  glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
  glBindTexture(GL_TEXTURE_2D, ui->texID[13]);
  drawMesh(view, OBJ_GEARBOX, 1);
  glDisable(GL_TEXTURE_2D);
  glPopMatrix();


  /** step 2 - draw /movable/ objects **/

  /* base material of moveable objects */
  glMaterialfv(GL_FRONT, GL_AMBIENT, no_mat);
  glMaterialfv(GL_FRONT, GL_SPECULAR, mat_specular);
  glMaterialfv(GL_FRONT, GL_SHININESS, high_shininess);

  for (i = 0; i < TOTAL_OBJ; ++i) {
    float y = ui->ctrls[i].y;

    if (ui->ctrls[i].type == OBJ_DRAWBAR) { /* drawbar */
      y -= (float) vmap_val_to_midi(view, i) / 12.7;
    }

    glPushMatrix();
    glLoadIdentity();
    glScalef(SCALE, SCALE, SCALE);
    glTranslatef(ui->ctrls[i].x, y, 0.0f);
    glRotatef(180, 0, 1, 0);

    switch(ui->ctrls[i].type) {
      case OBJ_DIAL:
	glMaterialfv(GL_FRONT, GL_AMBIENT, mat_dial);
	glMaterialfv(GL_FRONT, GL_DIFFUSE, mat_dial);
	glMaterialfv(GL_FRONT, GL_EMISSION, no_mat);
	if (ui->ctrls[i].max == 0) {
	  glRotatef(
	      240.0 - (360.0 * rint(ui->ctrls[i].cur - ui->ctrls[i].min) / (1.0 + ui->ctrls[i].max - ui->ctrls[i].min))
	      , 0, 0, 1);
	} else {
	  glRotatef(
	      150.0 - (300.0 * rint(ui->ctrls[i].cur - ui->ctrls[i].min) / (ui->ctrls[i].max - ui->ctrls[i].min))
	      , 0, 0, 1);
	}
	break;
      case OBJ_SWITCH:
	glMaterialfv(GL_FRONT, GL_AMBIENT, mat_switch);
	glMaterialfv(GL_FRONT, GL_DIFFUSE, mat_switch);
	if (ui->ctrls[i].cur == ui->ctrls[i].max) {
	  glMaterialfv(GL_FRONT, GL_EMISSION, glow_red);
	} else {
	  glMaterialfv(GL_FRONT, GL_EMISSION, no_mat);
	}
	glRotatef((vmap_val_to_midi(view, i) < 64 ? -12 : 12.0), 1, 0, 0); // XXX
	break;
      case OBJ_DRAWBAR:
	glMaterialfv(GL_FRONT, GL_EMISSION, no_mat);
	switch(i) {
	  case 0:
	  case 1:
	  case 9:
	  case 10:
	  case 18:
	  case 19:
	    glMaterialfv(GL_FRONT, GL_AMBIENT, mat_drawbar_brown);
	    glMaterialfv(GL_FRONT, GL_DIFFUSE, mat_drawbar_brown);
	    break;
	  case 4:
	  case 6:
	  case 7:
	  case 13:
	  case 15:
	  case 16:
	    glMaterialfv(GL_FRONT, GL_AMBIENT, mat_drawbar_black);
	    glMaterialfv(GL_FRONT, GL_DIFFUSE, mat_drawbar_black);
	    break;
	  default:
	    glMaterialfv(GL_FRONT, GL_AMBIENT, mat_drawbar_white);
	    glMaterialfv(GL_FRONT, GL_DIFFUSE, mat_drawbar_white);
	    break;
	}
	break;
      case OBJ_LEVER:
	glMaterialfv(GL_FRONT, GL_AMBIENT, mat_dial);
	glMaterialfv(GL_FRONT, GL_DIFFUSE, mat_lever);
	glMaterialfv(GL_FRONT, GL_EMISSION, no_mat);
	glRotatef(
	    (-40.0 + 80.0 * rint(ui->ctrls[i].cur - ui->ctrls[i].min) / (ui->ctrls[i].max - ui->ctrls[i].min))
	    , 0, 1, 0);
	break;
      default:
	break;
    }

    if (ui->ctrls[i].texID > 0) {
      glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
      glEnable(GL_TEXTURE_2D);
      glBindTexture(GL_TEXTURE_2D, ui->texID[ui->ctrls[i].texID]);
    }

    drawMesh(view, ui->ctrls[i].type, 1);
    glDisable(GL_TEXTURE_2D);
    glPopMatrix();

    float x = ui->ctrls[i].x;
    if (i == 24) x += 1.375;
    if (i == 25) x -= 1.375;
    if (i == 31) x += 2.8;
    if (i == 32) x -= 2.8;
    if (i < 20)  y -= 0.4;

    if (ui->show_mm) {
      render_text(view, ui->ctrls[i].midinfo, x, y, 1.5f, 1);
    }
    if (ui->uiccbind == i) {
      render_text(view, "move slider", x, y-.8, 1.6f, 1);
    }
  }
}

static void reset_state_ccbind(PuglView* view) {
  B3ui* ui = (B3ui*)puglGetHandle(view);
  if (ui->uiccbind >= 0) {
    ui->uiccbind = -1;
    forge_message_str(ui, ui->uris.sb3_uimccset, "off");
  }
  puglPostRedisplay(view);
}

static void reset_state(PuglView* view) {
  B3ui* ui = (B3ui*)puglGetHandle(view);
  ui->dndid = -1;
  ui->pgm_sel = -1;
  ui->dir_sel = -1;
  ui->dir_scrollgrab = 0;
  ui->dir_scroll = 0;
  reset_state_ccbind(view);
}

static void
onKeyboard(PuglView* view, bool press, uint32_t key)
{
  B3ui* ui = (B3ui*)puglGetHandle(view);
  int queue_reshape = 0;
  if (!press) {
    return;
  }
  if (ui->textentry_active) {
    txtentry_handle(view, key);
    return;
  }
  //TODO filter keys depending on displaymode and popupmsg
  switch (key) {
    case 'a':
      if (ui->rot[0] > -55) { ui->rot[0] -= 5; queue_reshape = 1; }
      break;
    case 'd':
      if (ui->rot[0] <  55) { ui->rot[0] += 5; queue_reshape = 1; }
      break;
    case 'w':
      if (ui->rot[1] > -80) { ui->rot[1] -= 5; queue_reshape = 1; }
      break;
    case 'x':
      if (ui->rot[1] <  0)  { ui->rot[1] += 5; queue_reshape = 1; }
      break;
    case 'z':
      if (ui->rot[2] > -30) { ui->rot[2] -= 5; queue_reshape = 1; }
      break;
    case 'c':
      if (ui->rot[2] <  30) { ui->rot[2] += 5; queue_reshape = 1; }
      break;
    case '+':
      if (ui->scale < 1.5) { ui->scale += .025; queue_reshape = 1; }
      break;
    case '-':
      if (ui->scale > 0.5) { ui->scale -= .025; queue_reshape = 1; }
      break;
    case 'h':
      if (ui->off[0] > -.5) { ui->off[0] -= .025; queue_reshape = 1; }
      break;
    case 'l':
      if (ui->off[0] <  .5) { ui->off[0] += .025; queue_reshape = 1; }
      break;
    case 'j':
      if (ui->off[1] > -.5) { ui->off[1] -= .025; queue_reshape = 1; }
      break;
    case 'k':
      if (ui->off[1] <  .5) { ui->off[1] += .025; queue_reshape = 1; }
      break;
    case 's':
      ui->rot[0] = ui->rot[1] = ui->rot[2] = 0.0;
      ui->scale = 0.875;
      ui->off[0] = ui->off[1] = ui->off[2] = 0.0;
      queue_reshape = 1;
      break;
    case 'e':
      ui->scale = 0.9;
      ui->rot[0] = ui->rot[1] = -20;
      ui->off[0] = ui->off[2] = 0.0;
      ui->off[1] = -0.1f;
      ui->rot[2] = 0;
      queue_reshape = 1;
      break;
    case 'p':
      if (ui->displaymode == 0) ui->displaymode = 2;
      else if (ui->displaymode == 2) ui->displaymode = 0;
      queue_reshape = 1;
      reset_state(view);
      break;
    case '?':
      if (ui->displaymode == 0) ui->displaymode = 1;
      else if (ui->displaymode == 1) ui->displaymode = 0;
      queue_reshape = 1;
      reset_state(view);
      break;
    case 'P':
      if (ui->displaymode == 0) ui->displaymode = 3;
      else if (ui->displaymode == 3) ui->displaymode = 0;
      queue_reshape = 1;
      reset_state(view);
      break;
    case 27: // ESC
      if (ui->popupmsg) {
	free(ui->popupmsg);
	ui->popupmsg = NULL;
      } else {
	ui->displaymode = 0;
	reset_state(view);
      }
      queue_reshape = 1;
      break;
    case 'm':
      if (ui->show_mm) {
	ui->show_mm = 0;
      } else {
	int i;
	for (i = 0; i < TOTAL_OBJ; ++i) {
	  ui->ctrls[i].midinfo[0] = '\0';
	}
	forge_message_str(ui, ui->uris.sb3_uimccquery, NULL);
	ui->show_mm = 1;
      }
      puglPostRedisplay(view);
      break;
#if 0 // not ready, yet
    case 'X':
      if (ui->displaymode == 0) {
	dirlist(view, ui->curdir);
	ui->displaymode = 4;
      }
      else if (ui->displaymode == 4) ui->displaymode = 0;
      queue_reshape = 1;
      reset_state(view);
      break;
#endif
    default:
      break;
  }

  if (queue_reshape) {
    onReshape(view, ui->width, ui->height);
    puglPostRedisplay(view);
  }
}

static void
onScroll(PuglView* view, int x, int y, float dx, float dy)
{
  B3ui* ui = (B3ui*)puglGetHandle(view);
  float fx, fy;
  if (ui->popupmsg) return;
  if (ui->displaymode) return;
  if (ui->textentry_active) return;
  project_mouse(view, x, y, &fx, &fy);
  int i;
  for (i = 0; i < TOTAL_OBJ ; ++i) {
    if (MOUSEOVER(ui->ctrls[i], fx, fy)) {
      ui->dndval = ui->ctrls[i].cur + SIGNUM(dy);
      processMotion(view, i, 0, 0);
    }
  }
}

static void
onMotion(PuglView* view, int x, int y)
{
  B3ui* ui = (B3ui*)puglGetHandle(view);
  float fx, fy;

  if (ui->textentry_active || ui->popupmsg) return;

  if (ui->displaymode == 2 || ui->displaymode == 3) {
    int pgm_sel = ui->pgm_sel;
    fx = (2.0 * x / ui->width ) - 1.0;
    fy = (2.0 * y / ui->height ) - 1.0;

    fx /= SCALE * 22.0; fy /= SCALE * 22.0;
    fx += 1.1; fy += 1.0;
    //printf("MX %fx%f --- %f %f \n", fx, fy,   fx * 2.7, fy*12);
    fx *= 2.7; fy *= 12.0;
    if (fx > 0 && fx < 6 && fy > 0 && fy < 24) {
      pgm_sel = floor(fx) * 24 + floor(fy);
    } else {
      pgm_sel = -1;
    }
    if (pgm_sel != ui->pgm_sel) {
      ui->pgm_sel = pgm_sel;
      puglPostRedisplay(view);
    }
    ui->dndid = -1;
    return;
  } else {
    ui->pgm_sel = -1;
  }

  if (ui->displaymode == 4 && ui->dir_scrollgrab) {
    fx = (2.0 * x / ui->width ) - 1.0;
    const int pages = (ui->dirlistlen / 20);
    const float ss = 1.6 / (float)pages;
    const int dir_scroll = ui->dir_scroll;
    ui->dir_scroll = (fx +.8) / ss;
    if (ui->dir_scroll < 0) ui->dir_scroll = 0;
    if (ui->dir_scroll > pages - 5) ui->dir_scroll = pages - 5;
    if (ui->dir_scroll != dir_scroll) {
      puglPostRedisplay(view);
    }
    return;
  }
  else if (ui->displaymode == 4) {
    int dir_sel = ui->dir_sel;
    fx = (2.0 * x / ui->width ) - 1.0;
    fy = (2.0 * y / ui->height ) - 1.0;

    fx /= SCALE * 22.0; fy /= SCALE * 22.0;
    fx += 1.1; fy += 1.0;
    fx *= 2.7; fy *= 12.0;
    fy+=1; fx+=ui->dir_scroll;
    if (fx > 0 && fy > 0 && fy < 20) {
      dir_sel = floor(fx) * 20 + floor(fy);
      if (dir_sel >= ui->dirlistlen) dir_sel = -1;
    } else {
      dir_sel = -1;
    }
    if (dir_sel != ui->dir_sel) {
      ui->dir_sel = dir_sel;
      puglPostRedisplay(view);
    }
    ui->dndid = -1;
    ui->pgm_sel = -1;
    return;
  } else {
    ui->dir_sel = -1;
  }

  if (ui->dndid < 0) return;

  project_mouse(view, x, y, &fx, &fy);

  const float dx = (fx - ui->dndx);
  const float dy = (fy - ui->dndy);

  processMotion(view, ui->dndid, dx, dy);
}

static void
onMouse(PuglView* view, int button, bool press, int x, int y)
{
  B3ui* ui = (B3ui*)puglGetHandle(view);
  int i;
  float fx, fy;
  project_mouse(view, x, y, &fx, &fy);
  //fprintf(stderr, "Mouse %d %s at %.3f,%.3f\n", button, press ? "down" : "up", fx, fy);

  if (!press) {
    ui->dndid = -1;
    ui->dir_scrollgrab = 0;
    return;
  }

  if (ui->textentry_active) return;

  if (ui->popupmsg) {
    free(ui->popupmsg);
    ui->popupmsg = NULL;
    onReshape(view, ui->width, ui->height);
    puglPostRedisplay(view);
    return;
  }

  if (ui->displaymode == 2) {
    ui->displaymode = 0;
    forge_message_int(ui, ui->uris.sb3_midipgm, ui->pgm_sel);
    onReshape(view, ui->width, ui->height);
    puglPostRedisplay(view);
    return;
  }

  if (ui->displaymode == 3) {
    if (ui->pgm_sel >= 0) {
      txtentry_start(view,"Enter Preset Name:", strlen(ui->midipgm[ui->pgm_sel]) > 0 ? ui->midipgm[ui->pgm_sel] : "User" );
    } else {
      ui->displaymode = 0;
    }
    onReshape(view, ui->width, ui->height);
    puglPostRedisplay(view);
    return;
  }

  if (ui->displaymode == 1) {
    ui->displaymode = 0;
    onReshape(view, ui->width, ui->height);
    puglPostRedisplay(view);
    return;
  }
  if (ui->displaymode == 4) {
    if (ui->dir_sel >= 0) {
      struct stat fs;
      char * rfn = absfilepath(ui->curdir, ui->dirlist[ui->dir_sel]);
      if(rfn && stat(rfn, &fs) == 0) {
	if (S_ISDIR(fs.st_mode)) {
	  free(ui->curdir);
	  ui->curdir = rfn;
	  dirlist(view, ui->curdir);
	  puglPostRedisplay(view);
	  return;
	} else if (S_ISREG(fs.st_mode)) {
	  int fnl = strlen(rfn);
	  if (fnl > 4 && !strcmp(&rfn[fnl-4], ".pgm")) {
	    forge_message_str(ui, ui->uris.sb3_loadpgm, rfn);
	  }
	  else if (fnl > 4 && !strcmp(&rfn[fnl-4], ".cfg")) {
	    forge_message_str(ui, ui->uris.sb3_loadcfg, rfn);
	  } else {
	    show_message(view, "file is not a .pgm nor .cfg");
	  }
	  free(rfn);
	}
      }
    } else if (ui->dirlistlen > 120) {
	// handle scrollbar
	fx = (2.0 * x / ui->width ) - 1.0;
	fy = (2.0 * y / ui->height ) - 1.0;
	if (fx >= -.8 && fx <= .8 && fy >= 0.625 && fy <= 0.7) {
	int pages = (ui->dirlistlen / 20);
	float ss = 1.6 / (float)pages;
	float sw = 5.0 * ss;
	float sx = ui->dir_scroll * ss - .8;
	if (fx < sx && ui->dir_scroll > 0) --ui->dir_scroll;
	else if (fx > sx+sw && ui->dir_scroll < (pages-4)) ++ui->dir_scroll;
	else if (fx >= sx && fx <= sx+sw) {
	  ui->dir_scrollgrab = 1;
	}
	ui->dir_sel = -1;
	puglPostRedisplay(view);
	return;
      }
    }
    ui->dir_sel = -1;
    ui->displaymode = 0;
    onReshape(view, ui->width, ui->height);
    puglPostRedisplay(view);
    return;
  }

  if (ui->displaymode == 0 && fx >= 1.050 && fx <= 1.150 && fy >= -.27 && fy <= -.19) {
    ui->displaymode = 1;
    onReshape(view, ui->width, ui->height);
    puglPostRedisplay(view);
    return;
  }

  if (puglGetModifiers(view) & PUGL_MOD_CTRL && button == 2) {
    for (i = 0; i < TOTAL_OBJ; ++i) {
      if (!MOUSEOVER(ui->ctrls[i], fx, fy)) {
	continue;
      }
      ui->uiccbind = i;
      forge_message_str(ui, ui->uris.sb3_uimccset, obj_control[i]);
      puglPostRedisplay(view);
      return;
    }
  }
  reset_state_ccbind(view);

  for (i = 0; i < TOTAL_OBJ; ++i) {
    if (!MOUSEOVER(ui->ctrls[i], fx, fy)) {
      continue;
    }

    switch (ui->ctrls[i].type) {
      case OBJ_DRAWBAR:
      case OBJ_DIAL:
      case OBJ_LEVER:
	ui->dndid = i;
	ui->dndx = fx;
	ui->dndy = fy;
	ui->dndval = ui->ctrls[i].cur;
	break;
      case OBJ_SWITCH:
	if (press) {
	  if (ui->ctrls[i].cur == ui->ctrls[i].max)
	    ui->ctrls[i].cur = ui->ctrls[i].min;
	  else
	    ui->ctrls[i].cur = ui->ctrls[i].max;
	  puglPostRedisplay(view);
	  notifyPlugin(view, i);
	}
	break;
      default:
	break;
    }
    break;
  }
}


/******************************************************************************
 * misc - used for LV2 init/operation
 */

#ifdef OLD_SUIL
static void* ui_thread(void* ptr)
{
  B3ui* ui = (B3ui*)ptr;
  while (!ui->exit) {
    usleep(1000000 / 25);  // 25 FPS
    puglProcessEvents(ui->view);
  }
  return NULL;
}
#else
static int idle(LV2UI_Handle handle) {
  B3ui* ui = (B3ui*)handle;
  puglProcessEvents(ui->view);
  return 0;
}
#endif


/******************************************************************************
 * main GUI setup
 */

static int sb3_gui_setup(B3ui* ui, const LV2_Feature* const* features) {
  PuglNativeWindow parent = 0;
  LV2UI_Resize*    resize = NULL;
  int i;

  ui->displaymode = 0;
  ui->pgm_sel     = -1;
  ui->show_mm     = 0;
  ui->uiccbind    = -1;
  ui->width       = 960;
  ui->height      = 320;
  ui->dndid       = -1;
  ui->initialized = 0;
  ui->textentry_active = 0;
  ui->dirlist     = NULL;
  ui->dirlistlen  = 0;
  ui->dir_sel     = -1;
  ui->dir_scroll  = 0;
  ui->dir_scrollgrab = 0;

  if (getenv("HOME")) {
    ui->curdir = strdup(getenv("HOME"));
  } else {
    ui->curdir = strdup("/");
  }

  ui->rot[0]     = -20;
  ui->rot[1]     = -20;
  ui->rot[2]     =  0.0;
  ui->scale      =  0.9;
  ui->off[0]     =  0.0f;
  ui->off[1]     = -0.1f;
  ui->off[2]     =  0.0f;

  for (int i = 0; features && features[i]; ++i) {
    if (!strcmp(features[i]->URI, LV2_UI__parent)) {
      parent = (PuglNativeWindow)features[i]->data;
    } else if (!strcmp(features[i]->URI, LV2_UI__resize)) {
      resize = (LV2UI_Resize*)features[i]->data;
    }
  }

  if (!parent) {
      fprintf(stderr, "B3Lv2UI error: No parent window provided.\n");
    return -1;
  }

  /* prepare meshes */
  ui->vbo = (GLuint *)malloc(OBJECTS_COUNT * sizeof(GLuint));
  ui->vinx = (GLuint *)malloc(OBJECTS_COUNT * sizeof(GLuint));

  /* Set up GL UI */
  ui->view = puglCreate(parent, "setBfree", ui->width, ui->height, true);
  puglSetHandle(ui->view, ui);
  puglSetDisplayFunc(ui->view, onDisplay);
  puglSetReshapeFunc(ui->view, onReshape);
  puglSetKeyboardFunc(ui->view, onKeyboard);
  puglSetMotionFunc(ui->view, onMotion);
  puglSetMouseFunc(ui->view, onMouse);
  puglSetScrollFunc(ui->view, onScroll);

  if (resize) {
    resize->ui_resize(resize->handle, ui->width, ui->height);
  }

  /** add control elements **/

#define CTRLELEM(ID, TYPE, VMIN, VMAX, VCUR, PX, PY, W, H, TEXID) \
  {\
    ui->ctrls[ID].type = TYPE; \
    ui->ctrls[ID].min = VMIN; \
    ui->ctrls[ID].max = VMAX; \
    ui->ctrls[ID].cur = VCUR; \
    ui->ctrls[ID].x = PX; \
    ui->ctrls[ID].y = PY; \
    ui->ctrls[ID].w = W; \
    ui->ctrls[ID].h = H; \
    ui->ctrls[ID].texID = TEXID; \
  }

  /* drawbars */
  for (i = 0; i < 9; ++i)
    CTRLELEM(i, OBJ_DRAWBAR, 0, 8, 0, 3.6 + 1.4 * i, 7, 1.2, 25, 1);
  for (; i < 18; ++i)
    CTRLELEM(i, OBJ_DRAWBAR, 0, 8, 0, -10.5 + 1.4 * (i-9), 7, 1.2, 25, 1);
  for (; i < 20; ++i)
    CTRLELEM(i, OBJ_DRAWBAR, 0, 8, 0, -14.8 + 1.4 * (i-18), 7, 1.2, 25, 1);

  /* btn - perc 20 - 23*/
  for (; i < 24; ++i)
    CTRLELEM(i, OBJ_SWITCH, 0, 1, 0, 18.75 + 2.75 * (i-20), -1, 2, 4, i-14);

  /* btn - vib 24, 25*/
  CTRLELEM(24, OBJ_SWITCH, 0, 1, 0, -21.50, -1, 2, 4, 3);
  CTRLELEM(25, OBJ_SWITCH, 0, 1, 0, -18.75, -1, 2, 4, 4);

  /* btn -- overdrive */
  CTRLELEM(26, OBJ_SWITCH, 0, 1, 0, -25.375, -1, 2, 4, 5);

  /* dials */
  CTRLELEM(27, OBJ_DIAL,  0, 127, -5,  -25.375, 3.5, 4, 4, 2); // overdrive
  CTRLELEM(28, OBJ_DIAL, -5, 0,   -5,  -20.375, 3.5, 4, 4, 2); // vibrato

  CTRLELEM(29, OBJ_DIAL,  0, 127,  0,   25.375, 3.5, 4, 4, 2); // volume
  CTRLELEM(30, OBJ_DIAL,  0, 127,  0,   20.375, 3.5, 4, 4, 2); // reverb

  /* Leslie levers */
  CTRLELEM(31, OBJ_LEVER, 0, 2, 2, -25.675, 8, 4, 3, -1);
  CTRLELEM(32, OBJ_LEVER, 0, 2, 0, -20.075, 8, 4, 3, -1);


#ifdef OLD_SUIL
  ui->exit = false;
  pthread_create(&ui->thread, NULL, ui_thread, ui);
#endif

  return 0;
}


/******************************************************************************
 * LV2 callbacks
 */

static LV2UI_Handle
instantiate(const LV2UI_Descriptor*   descriptor,
            const char*               plugin_uri,
            const char*               bundle_path,
            LV2UI_Write_Function      write_function,
            LV2UI_Controller          controller,
            LV2UI_Widget*             widget,
            const LV2_Feature* const* features)
{
  int i;
  B3ui* ui = (B3ui*)malloc(sizeof(B3ui));

  ui->map        = NULL;
  ui->write      = write_function;
  ui->controller = controller;

  for (i = 0; features[i]; ++i) {
    if (!strcmp(features[i]->URI, LV2_URID__map)) {
      ui->map = (LV2_URID_Map*)features[i]->data;
    }
  }

  if (!ui->map) {
    fprintf(stderr, "B3Lv2UI error: Host does not support urid:map\n");
    free(ui);
    return NULL;
  }

  map_setbfree_uris(ui->map, &ui->uris);
  lv2_atom_forge_init(&ui->forge, ui->map);

  if (sb3_gui_setup(ui, features)) {
    free(ui);
    return NULL;
  }

  memset(ui->midipgm, 0, 128 * 32 * sizeof(char));
  memset(ui->mididsc, 0, 128 * 256 * sizeof(char));

  *widget = (void*)puglGetNativeWindow(ui->view);

  /* ask plugin about current state */
  forge_message_str(ui, ui->uris.sb3_uiinit, NULL);

  return ui;
}

static void
cleanup(LV2UI_Handle handle)
{
  B3ui* ui = (B3ui*)handle;
#ifdef OLD_SUIL
  ui->exit = true;
  pthread_join(ui->thread, NULL);
#endif
  ftglDestroyFont(ui->font_big);
  ftglDestroyFont(ui->font_small);
  puglDestroy(ui->view);
  free(ui->vbo);
  free(ui->vinx);
  free(ui);
}

static void
port_event(LV2UI_Handle handle,
    uint32_t     port_index,
    uint32_t     buffer_size,
    uint32_t     format,
    const void*  buffer)
{
  B3ui* ui = (B3ui*)handle;
  char *k, *fn, *dsc; int v;

  if (format != ui->uris.atom_eventTransfer) {
    fprintf(stderr, "B3Lv2UI: Unknown message format.\n");
    return;
  }

  LV2_Atom* atom = (LV2_Atom*)buffer;

  if (atom->type == ui->uris.midi_MidiEvent) {
    return;
  }

  if (atom->type != ui->uris.atom_Blank) {
    fprintf(stderr, "B3Lv2UI: not an atom:Blank msg.\n");
    return;
  }

  LV2_Atom_Object* obj = (LV2_Atom_Object*)atom;
  if (!get_cc_key_value(&ui->uris, obj, &k, &v)) {
    if (!strcmp(k, "special.midimap")) {
      ui->uiccbind = -1;
      ui->show_mm = 0;
      puglPostRedisplay(ui->view);
    } else {
      processCCevent(ui, k, v);
    }
  } else if (!get_cc_midi_mapping(&ui->uris, obj, &fn, &k)) {
    int i;
    int fnid = -1;
    for (i = 0; i < TOTAL_OBJ; ++i) {
      if (!strcmp(obj_control[i], fn)) {fnid = i; break;}
    }
    if (fnid >= 0) {
      strcat(ui->ctrls[fnid].midinfo, k);
    }
    puglPostRedisplay(ui->view);
  } else if (!get_pgm_midi_mapping(&ui->uris, obj, &v, &fn, &dsc)) {
    strncpy(ui->midipgm[v], fn, 32);
    strncpy(ui->mididsc[v], dsc, 256);
    ui->midipgm[v][31] = '\0';
    ui->mididsc[v][255] = '\0';
    //printf("%d %s %s", v, ui->midipgm[v], ui->mididsc[v]);
    puglPostRedisplay(ui->view);
  } else if (obj->body.otype == ui->uris.sb3_loadcfg) {
    const LV2_Atom* msg = NULL;
    lv2_atom_object_get(obj, ui->uris.sb3_uimsg, &msg, 0);
    if (msg) {
      show_message(ui->view, (char*) LV2_ATOM_BODY(msg));
    }
  }
}

/******************************************************************************
 * LV2 setup
 */

#ifndef OLD_SUIL
static const LV2UI_Idle_Interface idle_iface = { idle };
#endif

static const void*
extension_data(const char* uri)
{
#ifndef OLD_SUIL
  if (!strcmp(uri, LV2_UI__idleInterface)) {
    return &idle_iface;
  }
#endif
  return NULL;
}

static const LV2UI_Descriptor descriptor = {
  SB3_URI "#ui",
  instantiate,
  cleanup,
  port_event,
  extension_data
};

LV2_SYMBOL_EXPORT
const LV2UI_Descriptor*
lv2ui_descriptor(uint32_t index)
{
  switch (index) {
  case 0:
    return &descriptor;
  default:
    return NULL;
  }
}

/* vi:set ts=8 sts=2 sw=2: */
