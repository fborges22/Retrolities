#include "disk.h"
#include <string.h>

void dt_map_range(const DtMap *m, unsigned cell, dt_u32 *first, dt_u32 *count)
{
    dt_u32 a, b;
    if (cell >= m->cells || !m->cells) {
        *first = 0;
        *count = 0;
        return;
    }
    a = (dt_u32)((dt_u64)m->count * cell / m->cells);
    b = (dt_u32)((dt_u64)m->count * (cell + 1) / m->cells);
    *first = m->first + a;
    *count = b - a;
}

static int init(DtMap *m, dt_u32 first, dt_u32 count, int surface)
{
    memset(m, 0, sizeof(*m));
    m->first = first;
    m->count = count;
    m->surface = surface;
    m->cells = count < DT_MAP_CELLS ? (unsigned)count : DT_MAP_CELLS;
    return count != 0;
}

int dt_map_build(DtVolume *v, dt_u32 first, dt_u32 count, DtMap *m, DtProgress cb, void *ctx)
{
    unsigned i;
    dt_u32 start, n, c, x, bad = v->bits == 12 ? 0xff7 : (v->bits == 16 ? 0xfff7 : 0xffffff7);
    memset(m, 0, sizeof(*m));
    if (first < 2 || first >= v->clusters + 2 || count > v->clusters + 2 - first ||
        !init(m, first, count, 0))
        return 0;
    for (i = 0; i < m->cells; i++) {
        DtMapCell *cell = &m->cell[i];
        dt_map_range(m, i, &start, &n);
        for (c = 0; c < n; c++) {
            if (!(c & 31) && cb && !cb(ctx, "Allocation map", start - first + c, count))
                return 0;
            if (!dt_fat(v, start + c, &x))
                cell->unreadable++;
            else if (!x)
                cell->free++;
            else if (x == bad)
                cell->bad++;
            else if ((x >= bad - 7 && x < bad) || ((x < 2 || x >= v->clusters + 2) && x < bad + 1))
                cell->unreadable++;
            else
                cell->used++;
            cell->done++;
        }
    }
    m->complete = 1;
    if (cb)
        cb(ctx, "Allocation map", count, count);
    return 1;
}

int dt_surface_ex(DtVolume *v, dt_u32 first, dt_u32 count, DtMap *m, DtProgress cb, void *ctx,
                  DtReport report, void *report_ctx)
{
    unsigned i;
    dt_u32 start, n, c;
    unsigned char b[512];

    memset(m, 0, sizeof(*m));
    if (first >= v->total || count > v->total - first || !init(m, first, count, 1))
        return 0;
    for (i = 0; i < m->cells; i++) {
        DtMapCell *cell = &m->cell[i];
        dt_map_range(m, i, &start, &n);
        for (c = 0; c < n; c++) {
            if (!(c & 31) && cb && !cb(ctx, "Surface read", start - first + c, count))
                return 0;
            if (dt_read(v, start + c, b))
                cell->used++;
            else {
                dt_u32 sector = start + c;
                char message[160];

                cell->unreadable++;
                if (report) {
                    if (sector >= v->data && (sector - v->data) / v->spc < v->clusters)
                        sprintf(message,
                                "Volume sector %lu; data cluster %lu, sector %u; read failed",
                                (unsigned long)sector,
                                (unsigned long)(2 + (sector - v->data) / v->spc),
                                (unsigned)((sector - v->data) % v->spc));
                    else
                        sprintf(message,
                                "Volume sector %lu; %s; read failed",
                                (unsigned long)sector,
                                sector < v->fat    ? "reserved metadata"
                                : sector < v->root ? "FAT metadata"
                                : sector < v->data ? "fixed root directory"
                                                   : "trailing volume space");
                    report(report_ctx, "SURFACE_READ", v->start + sector, message);
                }
            }
            cell->done++;
        }
    }
    m->complete = 1;
    if (cb)
        cb(ctx, "Surface read", count, count);
    return 1;
}

int dt_surface(DtVolume *v, dt_u32 first, dt_u32 count, DtMap *m, DtProgress cb, void *ctx)
{
    return dt_surface_ex(v, first, count, m, cb, ctx, NULL, NULL);
}
