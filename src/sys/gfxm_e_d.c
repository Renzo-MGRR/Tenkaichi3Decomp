#include "common.h"
#include "sys/gfxm_e.h"

/*
 * Relocation of the stage file, 0x114B18..0x115170. See include/sys/gfxm_e.h.
 *
 * BtlStage_Init (stg_a.c) points gBtlStage at the stage's header and calls BtlStage_Relocate with the base the
 * file's offsets are counted from. Every offset is in WORDS (pointer = base + offset * 4). The code that reads
 * the tables is the stage model code that follows (stg_d_b.c onwards) and stg_a.c / stg_b.c.
 */

extern StgRelocHdr *gBtlStage;

#define STG_RELOC(type, field) ((field) = (type)(base + (s32)(field)))

/* Turns the offsets of an octree node into pointers, then does the same for its eight children. */
void StgOctree_Relocate(StgOctNode *node, s32 *base) {
    s32 i;
    s32 j;
    s32 k;

    if (node->unk00 != NULL) {
        STG_RELOC(s32 *, node->unk00);
    }
    if (node->list != NULL) {
        STG_RELOC(s32 **, node->list);
    }
    for (i = 0; i < node->count; i++) {
        if (node->list[i] != NULL && (s32)node->list[i] != -1) {
            STG_RELOC(s32 *, node->list[i]);
        } else {
            node->list[i] = NULL;
        }
    }
    for (i = 0; i < 2; i++) {
        for (j = 0; j < 2; j++) {
            for (k = 0; k < 2; k++) {
                if (node->child[i][j][k] != NULL) {
                    STG_RELOC(StgOctNode *, node->child[i][j][k]);
                    StgOctree_Relocate(node->child[i][j][k], base);
                }
            }
        }
    }
}

/* Turns every offset of the stage header and of its tables into a pointer, then relocates the stage's
   texture file. The header pointer is used before it is tested (original bug: the test can never help). */
void BtlStage_Relocate(s32 *base) {
    u32 i;
    s32 j;
    s32 k;

    gBtlStage->base = base;
    if (gBtlStage != NULL) {
        STG_RELOC(StgRelocTimer *, gBtlStage->timers);
        STG_RELOC(StgRelocA *, gBtlStage->a);
        STG_RELOC(StgRelocB *, gBtlStage->b);
        STG_RELOC(void *, gBtlStage->unk2C);
        STG_RELOC(StgRelocC *, gBtlStage->c);
        STG_RELOC(StgRelocD *, gBtlStage->d);
        STG_RELOC(void *, gBtlStage->unk40);
        STG_RELOC(StgRelocE *, gBtlStage->e);
        STG_RELOC(StgOctNode *, gBtlStage->tree);
        for (i = 0; i < gBtlStage->timerCount; i++) {
            StgRelocTimer *t = &gBtlStage->timers[i];

            STG_RELOC(void *, t->ptr);
        }
        for (i = 0; i < gBtlStage->aCount; i++) {
            StgRelocA *a = &gBtlStage->a[i];

            STG_RELOC(StgRelocA1 *, a->items);
            for (j = 0; j < a->count; j++) {
                StgRelocA1 *a1 = &a->items[j];

                if (a1->opt != NULL) {
                    STG_RELOC(void *, a1->opt);
                }
                STG_RELOC(StgRelocA2 *, a1->items);
                for (k = 0; k < a1->count; k++) {
                    StgRelocA2 *a2 = &a1->items[k];
                    s32 o8 = (s32)a2->ptr8;
                    s32 oC = (s32)a2->ptrC;

                    a2->ptr8 = base + o8;
                    a2->ptrC = base + oC;
                }
            }
        }
        for (i = 0; i < gBtlStage->bCount; i++) {
            StgRelocB *b = &gBtlStage->b[i];

            STG_RELOC(StgRelocB1 *, b->items);
            for (j = 0; (u32)j < b->count; j++) {
                StgRelocB1 *b1 = &b->items[j];

                if (b1->opt != NULL) {
                    STG_RELOC(void *, b1->opt);
                }
                STG_RELOC(void **, b1->list);
                for (k = 0; k < b1->count; k++) {
                    STG_RELOC(void *, b1->list[k]);
                }
            }
        }
        for (i = 0; i < gBtlStage->cCount; i++) {
            StgRelocC *c = &gBtlStage->c[i];

            STG_RELOC(void *, c->ptr44);
        }
        for (i = 0; i < gBtlStage->dCount; i++) {
            StgRelocD *d = &gBtlStage->d[i];

            {
                s32 o04 = (s32)d->ptr04;
                s32 o0C = (s32)d->ptr0C;
                s32 o10 = (s32)d->ptr10;
                s32 o18 = (s32)d->items;

                d->ptr04 = base + o04;
                d->ptr0C = base + o0C;
                d->ptr10 = base + o10;
                d->items = (StgRelocD1 *)(base + o18);
            }
            for (j = 0; j < d->count; j++) {
                StgRelocD1 *d1 = &d->items[j];

                STG_RELOC(void *, d1->ptr8);
            }
        }
        for (i = 0; i < gBtlStage->eCount; i++) {
            StgRelocE *e = &gBtlStage->e[i];

            STG_RELOC(void **, e->list);
            for (j = 0; (u32)j < e->count; j++) {
                STG_RELOC(void *, e->list[j]);
            }
            for (j = 0; j < 2; j++) {
                STG_RELOC(void *, e->ptr0C[j]);
                STG_RELOC(void *, e->ptr14[j]);
                STG_RELOC(void *, e->ptr1C[j]);
            }
        }
        StgOctree_Relocate(gBtlStage->tree, base);
        Res_RelocateOffsets(&gBtlStage->tex, (u8 *)base, (TexFile *)(gBtlStage->base + gBtlStage->texOfs));
    }
}

/*
 * PS3: the stage files are little-endian, the target is big-endian. BtlStage_ConvertLe turns every word the
 * stage code reads into the target's byte order, once per loaded file, BEFORE BtlStage_Relocate turns the
 * offsets into pointers. Only words that are read are swapped: the outer pack header (file[0..0x18]) stays
 * little-endian and its readers use Port_ReadLe32, and the a-table VIF chains and the texture file keep their
 * own format. Called from StgModel_InitStage.
 */

static u32 Stage_Swap32(u32 v) {
    return (v << 24) | ((v & 0x0000FF00) << 8) | ((v >> 8) & 0x0000FF00) | (v >> 24);
}

static void Stage_SwapWords(void *ptr, s32 count) {
    u32 *p = (u32 *)ptr;

    while (count-- > 0) {
        *p = Stage_Swap32(*p);
        p++;
    }
}

static void Stage_SwapHalf(u16 *p) {
    u16 v = *p;

    *p = (u16)((v << 8) | (v >> 8));
}

/* One collision mesh: a 12-word header, then vertices, polygons and nodes. The three offsets count words from
   the stage base and are read before the header is swapped. Meshes are shared between zones and records. */
#define STG_CONV_COL_MAX 512

static void Stage_ConvertColMesh(u8 *base, u32 ofs, u32 *seen, s32 *seenCount) {
    u32 *p;
    u32 nodeCount;
    u32 polyCount;
    u32 nodeOfs;
    u32 polyOfs;
    u32 vtxOfs;
    s32 i;

    if (ofs == 0) {
        return;
    }
    for (i = 0; i < *seenCount; i++) {
        if (seen[i] == ofs) {
            return;
        }
    }
    if (*seenCount < STG_CONV_COL_MAX) {
        seen[(*seenCount)++] = ofs;
    }
    p = (u32 *)(base + ofs * 4);
    nodeCount = Port_ReadLe32(&p[0]);
    polyCount = Port_ReadLe32(&p[1]);
    nodeOfs = Port_ReadLe32(&p[3]);
    polyOfs = Port_ReadLe32(&p[4]);
    vtxOfs = Port_ReadLe32(&p[5]);
    Stage_SwapWords(p, 12);
    if (vtxOfs != 0 && polyOfs > vtxOfs) {
        Stage_SwapWords(base + vtxOfs * 4, (s32)(polyOfs - vtxOfs));
    }
    if (polyCount != 0 && polyOfs != 0) {
        Stage_SwapWords(base + polyOfs * 4, (s32)(polyCount * 8));
    }
    if (nodeCount != 0 && nodeOfs != 0) {
        Stage_SwapWords(base + nodeOfs * 4, (s32)(nodeCount * 8));
    }
}

/* Octree nodes are packed between their part lists, so children are not aligned. Read the links before
   swapping the node, then recurse. The tree is a strict tree (verified for all stage files). */
static void Stage_ConvertOctree(u8 *base, u32 ofs, s32 depth) {
    u32 *node;
    u32 child[8];
    u32 count;
    u32 list;
    s32 i;

    if (ofs == 0 || depth > 32) {
        return;
    }
    node = (u32 *)(base + ofs * 4);
    for (i = 0; i < 8; i++) {
        child[i] = Port_ReadLe32(&node[1 + i]);
    }
    count = Port_ReadLe32(&node[0xE]);
    list = Port_ReadLe32(&node[0xF]);
    Stage_SwapWords(node, 16);
    if (list != 0 && count != 0) {
        Stage_SwapWords(base + list * 4, (s32)count);
    }
    for (i = 0; i < 8; i++) {
        if (child[i] != 0) {
            Stage_ConvertOctree(base, child[i], depth + 1);
        }
    }
}

/* The stage parameter block (StgDataA): a 42-word header of (count, byte offset) pairs and its twenty lists.
   The header layout was mapped from all stage files; a list is swapped only in the words its readers use. */
static void Stage_ConvertDataA(u8 *file, u32 dataOfs) {
    u8 *data;
    u32 *d;
    u32 count;
    u32 ofs;
    u32 i;
    s32 j;
    u32 *p;

    if (dataOfs == 0) {
        return;
    }
    data = file + dataOfs;
    d = (u32 *)data;
    if (Port_ReadLe32(&d[0]) != 0x4450414D) {
        return;
    }
    Stage_SwapWords(d, 42);
    for (i = 0; i < 20; i++) {
        count = d[2 + i * 2];
        ofs = d[3 + i * 2];
        if (ofs == 0 || count == 0) {
            continue;
        }
        p = (u32 *)(data + ofs);
        switch (i) {
        case 0: /* objDefs, 0x40 bytes each */
            Stage_SwapWords(p, (s32)(count * 16));
            break;
        case 1: /* env, 0x10 */
            Stage_SwapWords(p, (s32)(count * 4));
            break;
        case 2: /* light, 0x20 */
            Stage_SwapWords(p, (s32)(count * 8));
            break;
        case 3: /* points, 0x10 */
            Stage_SwapWords(p, (s32)(count * 4));
            break;
        case 4: /* starts, 0x20 */
        case 5: /* altStarts, 0x20 */
        case 6: /* places, 0x20 */
            Stage_SwapWords(p, (s32)(count * 8));
            break;
        case 7: /* list40, 0x10: flags word only */
            for (j = 0; j < (s32)count; j++, p += 4) {
                Stage_SwapWords(p, 1);
            }
            break;
        case 8: /* list48, 0xC: two words */
            for (j = 0; j < (s32)count; j++, p += 3) {
                Stage_SwapWords(p, 2);
            }
            break;
        case 9: /* list50, 0x10: fog flags word only */
            for (j = 0; j < (s32)count; j++, p += 4) {
                Stage_SwapWords(p, 1);
            }
            break;
        case 10: /* list58, 0x20: five words and a u16 */
            for (j = 0; j < (s32)count; j++, p += 8) {
                Stage_SwapWords(p, 5);
                Stage_SwapHalf((u16 *)p + 10);
            }
            break;
        case 11: /* list60, 0x18: five words and two u16 */
            for (j = 0; j < (s32)count; j++, p += 6) {
                Stage_SwapWords(p, 5);
                Stage_SwapHalf((u16 *)p + 10);
                Stage_SwapHalf((u16 *)p + 11);
            }
            break;
        case 12: /* list68, 0x18: word 0, words 3-4 and a u16 */
            for (j = 0; j < (s32)count; j++, p += 6) {
                Stage_SwapWords(p, 1);
                Stage_SwapWords(p + 3, 2);
                Stage_SwapHalf((u16 *)p + 10);
            }
            break;
        case 17: /* list90, 8 bytes: flags word only */
            for (j = 0; j < (s32)count; j++, p += 2) {
                Stage_SwapWords(p, 1);
            }
            break;
        case 18: /* list98, 4 bytes: one word */
            for (j = 0; j < (s32)count; j++, p++) {
                Stage_SwapWords(p, 1);
            }
            break;
        case 19: /* listA0, 0x18: word 0 and the float at +0x14 */
            for (j = 0; j < (s32)count; j++, p += 6) {
                Stage_SwapWords(p, 1);
                Stage_SwapWords(p + 5, 1);
            }
            break;
        default: /* list70, list78, list80, list88 hold bytes only */
            break;
        }
    }
}

/* The path table: header words 1-4, then 0x10-byte entries and their 0x30-byte points. Offsets are bytes from
   the table. */
static void Stage_ConvertPaths(u8 *file, u32 tableOfs) {
    u32 *t;
    u32 count;
    u32 pathsOfs;
    u32 *paths;
    u32 i;
    s32 j;

    if (tableOfs == 0) {
        return;
    }
    t = (u32 *)(file + tableOfs);
    if (Port_ReadLe32(&t[0]) != 0x70505645) {
        return;
    }
    count = Port_ReadLe32(&t[2]);
    pathsOfs = Port_ReadLe32(&t[3]);
    Stage_SwapWords(t + 1, 4);
    if (pathsOfs == 0 || count == 0) {
        return;
    }
    paths = (u32 *)((u8 *)t + pathsOfs);
    for (i = 0; i < count; i++) {
        u32 *path = paths + i * 4;
        u32 end = Port_ReadLe32(&path[0]);
        u32 ptsOfs = Port_ReadLe32(&path[1]);

        Stage_SwapWords(path, 4);
        if (ptsOfs != 0 && end != 0) {
            u8 *pts = (u8 *)t + ptsOfs;

            for (j = 0; j < (s32)end; j++) {
                Stage_SwapWords(pts + j * 0x30, 8);
            }
        }
    }
}

/* The optional MEF0 block: three lists of 0x30-byte records with different u16 tails. */
static void Stage_ConvertMef(u8 *file, u32 mefOfs) {
    u32 *m;
    u32 count[3];
    u32 list[3];
    u32 i;
    s32 j;

    if (mefOfs == 0) {
        return;
    }
    m = (u32 *)(file + mefOfs);
    if (Port_ReadLe32(&m[0]) != 0x3046454D) {
        return;
    }
    for (i = 0; i < 3; i++) {
        count[i] = Port_ReadLe32(&m[2 + i * 2]);
        list[i] = Port_ReadLe32(&m[3 + i * 2]);
    }
    Stage_SwapWords(m + 1, 7);
    for (i = 0; i < 3; i++) {
        u8 *pts;

        if (count[i] == 0 || list[i] == 0) {
            continue;
        }
        pts = (u8 *)m + list[i];
        for (j = 0; j < (s32)count[i]; j++) {
            u32 *rec = (u32 *)(pts + j * 0x30);

            Stage_SwapWords(rec, 6);
            Stage_SwapHalf((u16 *)rec + 12);
            if (i < 2) {
                Stage_SwapHalf((u16 *)rec + 13);
            }
            if (i == 0) {
                Stage_SwapHalf((u16 *)rec + 14);
            }
        }
    }
}

/* The navigation graph: 4-word header, then 0x30-byte nodes (eight u32 then eight s16 links). */
static void Stage_ConvertNav(u8 *file, u32 navOfs) {
    u32 *nav;
    u32 count;
    u8 *node;
    u32 i;
    s32 j;

    if (navOfs == 0) {
        return;
    }
    nav = (u32 *)(file + navOfs);
    if (Port_ReadLe32(&nav[0]) != 1) {
        return;
    }
    count = Port_ReadLe32(&nav[1]);
    Stage_SwapWords(nav, 4);
    if (count == 0) {
        return;
    }
    node = (u8 *)nav + 0x10;
    for (i = 0; i < count; i++, node += 0x30) {
        Stage_SwapWords(node, 8);
        for (j = 0; j < 8; j++) {
            Stage_SwapHalf((u16 *)node + 16 + j);
        }
    }
}

/* One CMAn demo-camera block: header words 1-8, the eight default floats, the channels and their keys. */
static void Stage_ConvertDemoCam(u8 *file, u32 camOfs) {
    u32 *c;
    u32 chanOfs;
    u32 chanCount;
    u32 defOfs;
    u8 *ch;
    u32 keyOfs;
    u32 keyCount;
    u32 i;
    s32 j;

    if (camOfs == 0) {
        return;
    }
    c = (u32 *)(file + camOfs);
    if (Port_ReadLe32(&c[0]) != 0x6E414D43) {
        return;
    }
    chanOfs = Port_ReadLe32(&c[2]);
    chanCount = Port_ReadLe32(&c[3]);
    defOfs = Port_ReadLe32(&c[6]);
    Stage_SwapWords(c + 1, 8);
    if (defOfs != 0) {
        Stage_SwapWords((u8 *)c + defOfs, 8);
    }
    if (chanOfs == 0 || chanCount == 0) {
        return;
    }
    ch = (u8 *)c + chanOfs;
    for (i = 0; i < chanCount; i++, ch += 0x10) {
        u32 *chan = (u32 *)ch;

        keyOfs = Port_ReadLe32(&chan[1]);
        keyCount = Port_ReadLe32(&chan[2]);
        Stage_SwapWords(chan, 4);
        if (keyOfs != 0 && keyCount != 0) {
            for (j = 0; j < (s32)keyCount; j++) {
                Stage_SwapWords((u8 *)c + keyOfs + j * 0x20, 8);
            }
        }
    }
}

/* Converts one loaded stage file. Idempotent: the header's version word is 0x12 in the file and cannot look
   like 0x12 again through Port_ReadLe32 once swapped. */
void BtlStage_ConvertLe(u32 *file, void *stage) {
    u32 *hdr;
    u8 *base;
    u32 seen[STG_CONV_COL_MAX];
    s32 seenCount;
    u32 timerCount;
    u32 aCount;
    u32 bCount;
    u32 cCount;
    u32 dCount;
    u32 eCount;
    u32 dataOfs;
    u32 navOfs;
    u32 mefOfs;
    u32 pathOfs;
    u32 i;
    s32 j;
    s32 k;
    u32 *p;

    if (file == NULL || stage == NULL) {
        return;
    }
    hdr = (u32 *)stage;
    if (Port_ReadLe32(&hdr[0]) != 0x12) {
        return;
    }
    base = (u8 *)stage;
    seenCount = 0;

    /* Blocks addressed by the pack header, which stays little-endian. */
    dataOfs = Port_ReadLe32(&file[2]);
    navOfs = Port_ReadLe32(&file[0x10]);
    mefOfs = Port_ReadLe32(&file[0x13]);
    pathOfs = Port_ReadLe32(&file[0x18]);

    /* The stage header (24 words). */
    Stage_SwapWords(hdr, 24);
    timerCount = hdr[0x10];
    aCount = hdr[0x18];
    bCount = hdr[0x20];
    cCount = hdr[0x30];
    dCount = hdr[0x38];
    eCount = hdr[0x44];

    /* The animation/timer table (also the material animation table on stages 372, 376 and 395). */
    if (timerCount != 0 && hdr[0x14] != 0) {
        p = (u32 *)(base + hdr[0x14] * 4);
        for (i = 0; i < timerCount; i++) {
            u32 *t = p + i * 4;
            u32 framesOfs = Port_ReadLe32(&t[1]);
            u32 frameCount = Port_ReadLe16((u8 *)t + 2);

            Stage_SwapHalf((u16 *)t);
            Stage_SwapWords(t + 1, 3);
            if (framesOfs != 0 && frameCount != 0) {
                Stage_SwapWords(base + framesOfs * 4, (s32)frameCount);
            }
        }
    }

    /* a table: groups -> materials -> 0x10-byte mesh records. */
    if (aCount != 0 && hdr[0x1C] != 0) {
        u32 *a = (u32 *)(base + hdr[0x1C] * 4);

        for (i = 0; i < aCount; i++) {
            u32 *g = a + i * 2;
            u32 matCount = Port_ReadLe16((u8 *)g + 2);
            u32 matsOfs = Port_ReadLe32(&g[1]);
            u32 *mats;

            Stage_SwapHalf((u16 *)g);
            Stage_SwapHalf((u16 *)g + 1);
            Stage_SwapWords(&g[1], 1);
            if (matCount == 0 || matsOfs == 0) {
                continue;
            }
            mats = (u32 *)(base + matsOfs * 4);
            for (j = 0; j < (s32)matCount; j++) {
                u32 *m = mats + j * 4;
                u32 meshCount = Port_ReadLe16((u8 *)m + 2);
                u32 meshesOfs = Port_ReadLe32(&m[1]);
                u32 *mesh;

                Stage_SwapHalf((u16 *)m);
                Stage_SwapHalf((u16 *)m + 1);
                Stage_SwapWords(&m[1], 2);
                Stage_SwapHalf((u16 *)m + 6);
                Stage_SwapHalf((u16 *)m + 7);
                if (meshCount == 0 || meshesOfs == 0) {
                    continue;
                }
                mesh = (u32 *)(base + meshesOfs * 4);
                for (k = 0; k < (s32)meshCount; k++) {
                    u32 *r = mesh + k * 4;

                    Stage_SwapHalf((u16 *)r);
                    Stage_SwapWords(&r[2], 2);
                }
            }
        }
    }

    /* b table: lists of inline 0x20-byte parts, each with a mesh-pointer array. */
    if (bCount != 0 && hdr[0x24] != 0) {
        u32 *b = (u32 *)(base + hdr[0x24] * 4);

        for (i = 0; i < bCount; i++) {
            u32 *list = b + i * 2;
            u32 partCount = Port_ReadLe32(&list[0]);
            u32 partsOfs = Port_ReadLe32(&list[1]);
            u32 *parts;

            Stage_SwapWords(list, 2);
            if (partCount == 0 || partsOfs == 0) {
                continue;
            }
            parts = (u32 *)(base + partsOfs * 4);
            for (j = 0; j < (s32)partCount; j++) {
                u32 *part = parts + j * 8;
                u32 meshCount = Port_ReadLe16((u8 *)part);
                u32 meshesOfs = Port_ReadLe32(&part[1]);

                Stage_SwapHalf((u16 *)part);
                Stage_SwapHalf((u16 *)part + 1);
                Stage_SwapWords(&part[1], 7);
                if (meshCount != 0 && meshesOfs != 0) {
                    Stage_SwapWords(base + meshesOfs * 4, (s32)meshCount);
                }
            }
        }
    }

    /* c table: 0x50-byte transform nodes and their 0x20-byte keys. */
    if (cCount != 0 && hdr[0x34] != 0) {
        u32 *c = (u32 *)(base + hdr[0x34] * 4);

        for (i = 0; i < cCount; i++) {
            u32 *node = c + i * 20;
            u32 keyCount = Port_ReadLe32(&node[0x10]);
            u32 keysOfs = Port_ReadLe32(&node[0x11]);

            Stage_SwapWords(node, 20);
            if (keyCount != 0 && keysOfs != 0) {
                Stage_SwapWords(base + keysOfs * 4, (s32)(keyCount * 8));
            }
        }
    }

    /* d table: 0x40-byte zones, their near-index arrays and 0x10-byte collision records. */
    if (dCount != 0 && hdr[0x3C] != 0) {
        u32 *d = (u32 *)(base + hdr[0x3C] * 4);

        for (i = 0; i < dCount; i++) {
            u32 *zone = d + i * 16;
            u32 meshOfs = Port_ReadLe32(&zone[1]);
            u32 nearCount = Port_ReadLe32(&zone[2]);
            u32 nearOfs = Port_ReadLe32(&zone[3]);
            u32 recCount = Port_ReadLe32(&zone[5]);
            u32 recOfs = Port_ReadLe32(&zone[6]);

            Stage_SwapWords(zone, 16);
            if (nearCount != 0 && nearOfs != 0) {
                Stage_SwapWords(base + nearOfs * 4, (s32)nearCount);
            }
            if (recCount != 0 && recOfs != 0) {
                u32 *rec = (u32 *)(base + recOfs * 4);

                for (j = 0; j < (s32)recCount; j++) {
                    u32 *r = rec + j * 4;
                    u32 rmesh = Port_ReadLe32(&r[2]);

                    Stage_SwapWords(r, 4);
                    Stage_ConvertColMesh(base, rmesh, seen, &seenCount);
                }
            }
            Stage_ConvertColMesh(base, meshOfs, seen, &seenCount);
        }
    }

    /* e table: 0x50-byte objects and their lists of b-table part offsets (the parts are already swapped). */
    if (eCount != 0 && hdr[0x48] != 0) {
        u32 *e = (u32 *)(base + hdr[0x48] * 4);

        for (i = 0; i < eCount; i++) {
            u32 *obj = e + i * 20;
            u32 pieceCount = Port_ReadLe32(&obj[9]);
            u32 piecesOfs = Port_ReadLe32(&obj[10]);

            Stage_SwapWords(obj, 20);
            if (pieceCount != 0 && piecesOfs != 0) {
                Stage_SwapWords(base + piecesOfs * 4, (s32)pieceCount);
            }
        }
    }

    /* The octree and the stage bounds. */
    if (hdr[0x50] != 0) {
        Stage_ConvertOctree(base, hdr[0x50], 0);
    }
    if (hdr[0x40] != 0) {
        Stage_SwapWords(base + hdr[0x40] * 4, 3);
    }

    /* Blocks outside the header. */
    Stage_ConvertDataA((u8 *)file, dataOfs);
    Stage_ConvertPaths((u8 *)file, pathOfs);
    Stage_ConvertMef((u8 *)file, mefOfs);
    Stage_ConvertNav((u8 *)file, navOfs);
    Stage_ConvertDemoCam((u8 *)file, Port_ReadLe32(&file[3]));
    Stage_ConvertDemoCam((u8 *)file, Port_ReadLe32(&file[4]));
    Stage_ConvertDemoCam((u8 *)file, Port_ReadLe32(&file[5]));
}
