/*
 * gui_chart_draw.h - 图表坐标、阈值、统计与边框
 */

#ifndef FMON_GUI_CHART_DRAW_H
#define FMON_GUI_CHART_DRAW_H

#include "gui_chart.h"
#include "chart_geom.h"

bool gui_chart_curve_range(const CurveData *cd, uint32_t points, uint64_t t0,
                           uint64_t t1, bool time_window, double *lo,
                           double *hi);
bool gui_chart_nearest_sample_by_time(const CurveData *cd, uint32_t points,
                                      uint64_t t, uint32_t *out_idx);

gboolean gui_chart_auto_range_for(const CurveData *cd);
void     gui_chart_set_font(cairo_t *cr);
gboolean gui_chart_curve_threshold(const CurveData *cd, double *value,
                                   char *label, size_t label_len);
void     gui_chart_draw_hint(cairo_t *cr, int left, int top, int plot_w,
                             int plot_h, const char *text);
uint64_t gui_chart_gap_threshold_us(const CurveData *cd);
void     gui_chart_draw_threshold_markers(cairo_t *cr, const CurveData *cd,
                                          const FmonChartScale *sc,
                                          const FmonChartPoint *pts, size_t n,
                                          int margin_top, int plot_h);
void     gui_chart_draw_threshold(cairo_t *cr, const CurveData *cd,
                                  const FmonChartScale *sc, int margin_left,
                                  int margin_top, int plot_w, int plot_h);
void     gui_chart_draw_stats(cairo_t *cr, const CurveData *cd,
                              uint32_t points, uint64_t t0, uint64_t t1,
                              bool time_window, int margin_left,
                              int margin_top, int plot_w);
void     gui_chart_draw_frame(cairo_t *cr, const CurveData *cd,
                              const FmonChartScale *sc, int margin_left,
                              int margin_top, int plot_w, int plot_h);

#endif
