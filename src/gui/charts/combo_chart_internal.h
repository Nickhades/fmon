/*
 * combo_chart_internal.h - 组合图表内部共享类型与函数
 */

#ifndef FMON_COMBO_CHART_INTERNAL_H
#define FMON_COMBO_CHART_INTERNAL_H

#include "combo_chart.h"

#include <cairo.h>

#define COMBO_MAX_POINTS  FMON_HISTORY_POINTS
#define COMBO_TAU         6.28318530717958647692

typedef struct {
    double left;
    double right;
    double top;
    double bottom;
    double plot_w;
    double plot_h;
} ComboGeom;

typedef struct {
    double top;
    double bottom;
    double plot_top;
    double plot_h;
} BandGeom;

double series_value(const CurveData *cd, uint32_t i);
void series_range(const CurveData *cd, double *lo, double *hi);
void series_text_color(const CurveData *cd, double *r, double *g, double *b);
void format_value_text(char *buf, size_t n, double v, const char *unit);
void combo_geom_init(ComboGeom *g, int width, int height);
void band_layout(const ComboGeom *g, int series_count, int s, BandGeom *b);
double band_y(const BandGeom *b, double v, double lo, double hi);
void set_font(cairo_t *cr, double size);
void format_span(char *buf, size_t n, int64_t secs);

void draw_grid(cairo_t *cr, const ComboGeom *g, int series_count);
void draw_axis_labels(cairo_t *cr, const ComboGeom *g, const ComboChart *c);
void draw_band_frame(cairo_t *cr, const ComboGeom *g, const BandGeom *b, const CurveData *cd);
void draw_band_label(cairo_t *cr, const ComboGeom *g, const BandGeom *b, const ComboSeries *series, const CurveData *cd);
void draw_series_fill(cairo_t *cr, const ComboGeom *g, const BandGeom *b, const CurveData *cd, double lo, double hi);
void draw_series_line(cairo_t *cr, const ComboGeom *g, const BandGeom *b, const CurveData *cd, double lo, double hi);
void draw_hover_line(cairo_t *cr, const ComboGeom *g, double x);
void draw_hover_dots(cairo_t *cr, const ComboGeom *g, const ComboChart *c, uint32_t idx, double x);
void draw_hover_box(cairo_t *cr, const ComboGeom *g, const ComboChart *c, uint32_t idx);
void draw_hover(cairo_t *cr, const ComboGeom *g, const ComboChart *c);

#endif /* FMON_COMBO_CHART_INTERNAL_H */
