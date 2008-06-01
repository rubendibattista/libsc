/*
  This file is part of p4est.
  p4est is a C library to manage a parallel collection of quadtrees and/or
  octrees.

  Copyright (C) 2007,2008 Carsten Burstedde, Lucas Wilcox.

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <p4est_algorithms.h>
#include <p4est_communication.h>

/* htonl is in either of these two */
#ifdef P4EST_HAVE_ARPA_NET_H
#include <arpa/inet.h>
#endif
#ifdef P4EST_HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

/* *INDENT-OFF* */

/** The offsets of the 3 indirect neighbors in units of h.
 * Indexing [cid][neighbor][xy] where cid is the child id.
 * Neighbors are indexed in z-order.
 */
static const int    indirect_neighbors[4][3][2] =
{{{-1, -1}, { 1, -1}, {-1, 1}},
 {{ 0, -1}, { 2, -1}, { 1, 0}},
 {{-1,  0}, {-2,  1}, { 0, 1}},
 {{ 1, -1}, {-1,  1}, { 1, 1}}};

/** Indicate which neighbor to omit if edges are balanced, not corners
 * Indexing [cid] where cid is the child id.
 */
static const int    corners_omitted[4] =
{0, 1, 1, 2};

/* *INDENT-ON* */

/* here come small auxiliary functions */

int
p4est_quadrant_compare (const void *v1, const void *v2)
{
  const p4est_quadrant_t *q1 = v1;
  const p4est_quadrant_t *q2 = v2;

  uint32_t            exclorx, exclory;
  int64_t             p1, p2, diff;

  P4EST_ASSERT (p4est_quadrant_is_extended (q1));
  P4EST_ASSERT (p4est_quadrant_is_extended (q2));

  /* these are unsigned variables that inherit the sign bits */
  exclorx = q1->x ^ q2->x;
  exclory = q1->y ^ q2->y;

  if (exclory == 0 && exclorx == 0) {
    return (int) q1->level - (int) q2->level;
  }
  else if (SC_LOG2_32 (exclory) >= SC_LOG2_32 (exclorx)) {
    p1 = q1->y + ((q1->y >= 0) ? 0 : ((int64_t) 1 << (P4EST_MAXLEVEL + 2)));
    p2 = q2->y + ((q2->y >= 0) ? 0 : ((int64_t) 1 << (P4EST_MAXLEVEL + 2)));
    diff = p1 - p2;
    return (diff == 0) ? 0 : ((diff < 0) ? -1 : 1);
  }
  else {
    p1 = q1->x + ((q1->x >= 0) ? 0 : ((int64_t) 1 << (P4EST_MAXLEVEL + 2)));
    p2 = q2->x + ((q2->x >= 0) ? 0 : ((int64_t) 1 << (P4EST_MAXLEVEL + 2)));
    diff = p1 - p2;
    return (diff == 0) ? 0 : ((diff < 0) ? -1 : 1);
  }
}

int
p4est_quadrant_compare_piggy (const void *v1, const void *v2)
{
  const p4est_quadrant_t *q1 = v1;
  const p4est_quadrant_t *q2 = v2;

  /* expect non-negative tree information */
  const p4est_topidx_t diff = q1->p.piggy.which_tree - q2->p.piggy.which_tree;

  return (diff == 0) ?
    p4est_quadrant_compare (v1, v2) : ((diff < 0) ? -1 : 1);
}

int
p4est_quadrant_is_equal (const void *v1, const void *v2)
{
  const p4est_quadrant_t *q1 = v1;
  const p4est_quadrant_t *q2 = v2;

  P4EST_ASSERT (p4est_quadrant_is_extended (q1));
  P4EST_ASSERT (p4est_quadrant_is_extended (q2));

  return (q1->level == q2->level && q1->x == q2->x && q1->y == q2->y);
}

unsigned
p4est_quadrant_hash (const void *v)
{
  const p4est_quadrant_t *q = v;

  return (unsigned) (p4est_quadrant_linear_id (q, q->level) %
                     ((uint64_t) 1 << 30));
}

int
p4est_quadrant_child_id (const p4est_quadrant_t * q)
{
  int                 id = 0;

  P4EST_ASSERT (p4est_quadrant_is_extended (q));

  if (q->level == 0) {
    return 0;
  }

  id |= ((q->x & P4EST_QUADRANT_LEN (q->level)) ? 0x01 : 0);
  id |= ((q->y & P4EST_QUADRANT_LEN (q->level)) ? 0x02 : 0);

  return id;
}

int
p4est_quadrant_is_inside (const p4est_quadrant_t * q)
{
  return
    (q->x >= 0 && q->x < P4EST_ROOT_LEN) &&
    (q->y >= 0 && q->y < P4EST_ROOT_LEN);
}

int
p4est_quadrant_is_valid (const p4est_quadrant_t * q)
{
  return
    (q->level >= 0 && q->level <= P4EST_MAXLEVEL) &&
    (q->x >= 0 && q->x < P4EST_ROOT_LEN) &&
    (q->y >= 0 && q->y < P4EST_ROOT_LEN) &&
    ((q->x & (P4EST_QUADRANT_LEN (q->level) - 1)) == 0) &&
    ((q->y & (P4EST_QUADRANT_LEN (q->level) - 1)) == 0);
}

int
p4est_quadrant_is_extended (const p4est_quadrant_t * q)
{
  return
    (q->level >= 0 && q->level <= P4EST_MAXLEVEL) &&
    ((q->x & (P4EST_QUADRANT_LEN (q->level) - 1)) == 0) &&
    ((q->y & (P4EST_QUADRANT_LEN (q->level) - 1)) == 0);
}

int
p4est_quadrant_is_sibling (const p4est_quadrant_t * q1,
                           const p4est_quadrant_t * q2)
{
  p4est_qcoord_t      exclorx, exclory;

  P4EST_ASSERT (p4est_quadrant_is_extended (q1));
  P4EST_ASSERT (p4est_quadrant_is_extended (q2));

  if (q1->level == 0) {
    return 0;
  }

  exclorx = q1->x ^ q2->x;
  exclory = q1->y ^ q2->y;
  if (exclorx == 0 && exclory == 0) {
    return 0;
  }

  return
    (q1->level == q2->level) &&
    ((exclorx & ~P4EST_QUADRANT_LEN (q1->level)) == 0) &&
    ((exclory & ~P4EST_QUADRANT_LEN (q1->level)) == 0);
}

int
p4est_quadrant_is_sibling_D (const p4est_quadrant_t * q1,
                             const p4est_quadrant_t * q2)
{
  p4est_quadrant_t    p1, p2;

  /* make sure the quadrant_parent functions below don't abort */
  if (q1->level == 0) {
    return 0;
  }

  /* validity of q1 and q2 is asserted in p4est_quadrant_is_equal */
  if (p4est_quadrant_is_equal (q1, q2)) {
    return 0;
  }

  p4est_quadrant_parent (q1, &p1);
  p4est_quadrant_parent (q2, &p2);

  return p4est_quadrant_is_equal (&p1, &p2);
}

int
p4est_quadrant_is_family (const p4est_quadrant_t * q0,
                          const p4est_quadrant_t * q1,
                          const p4est_quadrant_t * q2,
                          const p4est_quadrant_t * q3)
{
  p4est_qcoord_t      inc;

  P4EST_ASSERT (p4est_quadrant_is_extended (q0));
  P4EST_ASSERT (p4est_quadrant_is_extended (q1));
  P4EST_ASSERT (p4est_quadrant_is_extended (q2));
  P4EST_ASSERT (p4est_quadrant_is_extended (q3));

  if (q0->level == 0 || q0->level != q1->level ||
      q0->level != q2->level || q0->level != q3->level) {
    return 0;
  }

  inc = P4EST_QUADRANT_LEN (q0->level);
  return ((q0->x + inc == q1->x && q0->y == q1->y) &&
          (q0->x == q2->x && q0->y + inc == q2->y) &&
          (q1->x == q3->x && q2->y == q3->y));
}

int
p4est_quadrant_is_parent (const p4est_quadrant_t * q,
                          const p4est_quadrant_t * r)
{
  P4EST_ASSERT (p4est_quadrant_is_extended (q));
  P4EST_ASSERT (p4est_quadrant_is_extended (r));

  return
    (q->level + 1 == r->level) &&
    (q->x == (r->x & ~P4EST_QUADRANT_LEN (r->level))) &&
    (q->y == (r->y & ~P4EST_QUADRANT_LEN (r->level)));
}

int
p4est_quadrant_is_parent_D (const p4est_quadrant_t * q,
                            const p4est_quadrant_t * r)
{
  p4est_quadrant_t    p;

  P4EST_ASSERT (p4est_quadrant_is_extended (q));

  /* make sure the quadrant_parent function below doesn't abort */
  if (r->level == 0) {
    return 0;
  }

  /* validity of r is asserted in p4est_quadrant_parent */
  p4est_quadrant_parent (r, &p);

  return p4est_quadrant_is_equal (q, &p);
}

int
p4est_quadrant_is_ancestor (const p4est_quadrant_t * q,
                            const p4est_quadrant_t * r)
{
  p4est_qcoord_t      exclorx;
  p4est_qcoord_t      exclory;

  P4EST_ASSERT (p4est_quadrant_is_extended (q));
  P4EST_ASSERT (p4est_quadrant_is_extended (r));

  if (q->level >= r->level) {
    return 0;
  }

  exclorx = (q->x ^ r->x) >> (P4EST_MAXLEVEL - q->level);
  exclory = (q->y ^ r->y) >> (P4EST_MAXLEVEL - q->level);

  return (exclorx == 0 && exclory == 0);
}

int
p4est_quadrant_is_ancestor_D (const p4est_quadrant_t * q,
                              const p4est_quadrant_t * r)
{
  p4est_quadrant_t    s;

  /* validity of q and r is asserted in p4est_quadrant_is_equal */
  if (p4est_quadrant_is_equal (q, r)) {
    return 0;
  }

  /* this will abort if q and r are in different trees */
  p4est_nearest_common_ancestor_D (q, r, &s);

  return p4est_quadrant_is_equal (q, &s);
}

int
p4est_quadrant_is_next (const p4est_quadrant_t * q,
                        const p4est_quadrant_t * r)
{
  int                 minlevel;
  uint64_t            i1, i2;
  p4est_qcoord_t      mask;

  P4EST_ASSERT (p4est_quadrant_is_extended (q));
  P4EST_ASSERT (p4est_quadrant_is_extended (r));

  /* the condition q < r is checked implicitly */

  if (q->level > r->level) {
    /* check if q is the last child up to the common level */
    mask = P4EST_QUADRANT_LEN (r->level) - P4EST_QUADRANT_LEN (q->level);
    if ((q->x & mask) != mask || (q->y & mask) != mask) {
      return 0;
    }
    minlevel = r->level;
  }
  else {
    minlevel = q->level;
  }
  i1 = p4est_quadrant_linear_id (q, minlevel);
  i2 = p4est_quadrant_linear_id (r, minlevel);

  return (i1 + 1 == i2);
}

int
p4est_quadrant_is_next_D (const p4est_quadrant_t * q,
                          const p4est_quadrant_t * r)
{
  uint64_t            i1, i2;
  p4est_quadrant_t    a, b;

  /* validity of q and r is asserted in p4est_quadrant_compare */
  if (p4est_quadrant_compare (q, r) >= 0) {
    return 0;
  }

  a = *q;
  b = *r;
  while (a.level > b.level) {
    if (p4est_quadrant_child_id (&a) != 3) {
      return 0;
    }
    p4est_quadrant_parent (&a, &a);
  }
  i1 = p4est_quadrant_linear_id (&a, a.level);
  i2 = p4est_quadrant_linear_id (&b, a.level);

  return (i1 + 1 == i2);
}

void
p4est_quadrant_parent (const p4est_quadrant_t * q, p4est_quadrant_t * r)
{
  P4EST_ASSERT (p4est_quadrant_is_extended (q));
  P4EST_ASSERT (q->level > 0);

  r->x = q->x & ~P4EST_QUADRANT_LEN (q->level);
  r->y = q->y & ~P4EST_QUADRANT_LEN (q->level);
  r->level = (int8_t) (q->level - 1);

  P4EST_ASSERT (p4est_quadrant_is_extended (r));
}

void
p4est_quadrant_sibling (const p4est_quadrant_t * q, p4est_quadrant_t * r,
                        int sibling_id)
{
  const int           addx = (sibling_id & 0x01);
  const int           addy = (sibling_id & 0x02) >> 1;
  const p4est_qcoord_t shift = P4EST_QUADRANT_LEN (q->level);

  P4EST_ASSERT (p4est_quadrant_is_extended (q));
  P4EST_ASSERT (q->level > 0);
  P4EST_ASSERT (sibling_id >= 0 && sibling_id < 4);

  r->x = (addx ? (q->x | shift) : (q->x & ~shift));
  r->y = (addy ? (q->y | shift) : (q->y & ~shift));
  r->level = q->level;
}

void
p4est_quadrant_children (const p4est_quadrant_t * q,
                         p4est_quadrant_t * c0, p4est_quadrant_t * c1,
                         p4est_quadrant_t * c2, p4est_quadrant_t * c3)
{
  P4EST_ASSERT (p4est_quadrant_is_extended (q));
  P4EST_ASSERT (q->level < P4EST_MAXLEVEL);

  c0->x = q->x;
  c0->y = q->y;
  c0->level = (int8_t) (q->level + 1);

  c1->x = c0->x | P4EST_QUADRANT_LEN (c0->level);
  c1->y = c0->y;
  c1->level = c0->level;

  c2->x = c0->x;
  c2->y = c0->y | P4EST_QUADRANT_LEN (c0->level);
  c2->level = c0->level;

  c3->x = c1->x;
  c3->y = c2->y;
  c3->level = c0->level;

  P4EST_ASSERT (p4est_quadrant_is_extended (c0));
  P4EST_ASSERT (p4est_quadrant_is_extended (c1));
  P4EST_ASSERT (p4est_quadrant_is_extended (c2));
  P4EST_ASSERT (p4est_quadrant_is_extended (c3));
}

void
p4est_quadrant_first_descendent (const p4est_quadrant_t * q,
                                 p4est_quadrant_t * fd, int level)
{
  P4EST_ASSERT (p4est_quadrant_is_extended (q));
  P4EST_ASSERT (q->level <= level && level <= P4EST_MAXLEVEL);

  fd->x = q->x;
  fd->y = q->y;
  fd->level = (int8_t) level;
}

void
p4est_quadrant_last_descendent (const p4est_quadrant_t * q,
                                p4est_quadrant_t * ld, int level)
{
  p4est_qcoord_t      shift;

  P4EST_ASSERT (p4est_quadrant_is_extended (q));
  P4EST_ASSERT (q->level <= level && level <= P4EST_MAXLEVEL);

  shift = P4EST_QUADRANT_LEN (q->level) - P4EST_QUADRANT_LEN (level);

  ld->x = q->x + shift;
  ld->y = q->y + shift;
  ld->level = (int8_t) level;
}

void
p4est_nearest_common_ancestor (const p4est_quadrant_t * q1,
                               const p4est_quadrant_t * q2,
                               p4est_quadrant_t * r)
{
  int                 maxlevel;
  uint32_t            exclorx, exclory, maxclor;

  P4EST_ASSERT (p4est_quadrant_is_extended (q1));
  P4EST_ASSERT (p4est_quadrant_is_extended (q2));

  exclorx = q1->x ^ q2->x;
  exclory = q1->y ^ q2->y;
  maxclor = exclorx | exclory;
  maxlevel = SC_LOG2_32 (maxclor) + 1;

  P4EST_ASSERT (maxlevel <= P4EST_MAXLEVEL);

  r->x = q1->x & ~((1 << maxlevel) - 1);
  r->y = q1->y & ~((1 << maxlevel) - 1);
  r->level = (int8_t) SC_MIN (P4EST_MAXLEVEL - maxlevel,
                              SC_MIN (q1->level, q2->level));

  P4EST_ASSERT (p4est_quadrant_is_extended (r));
}

void
p4est_nearest_common_ancestor_D (const p4est_quadrant_t * q1,
                                 const p4est_quadrant_t * q2,
                                 p4est_quadrant_t * r)
{
  p4est_quadrant_t    s1 = *q1;
  p4est_quadrant_t    s2 = *q2;

  P4EST_ASSERT (p4est_quadrant_is_extended (q1));
  P4EST_ASSERT (p4est_quadrant_is_extended (q2));

  /* first stage: promote the deepest one to the same level */
  while (s1.level > s2.level) {
    p4est_quadrant_parent (&s1, &s1);
  }
  while (s1.level < s2.level) {
    p4est_quadrant_parent (&s2, &s2);
  }

  /* second stage: simultaneously go through their parents */
  while (!p4est_quadrant_is_equal (&s1, &s2)) {
    p4est_quadrant_parent (&s1, &s1);
    p4est_quadrant_parent (&s2, &s2);
  }

  /* don't overwrite r's user_data */
  r->x = s1.x;
  r->y = s1.y;
  r->level = s1.level;

  P4EST_ASSERT (p4est_quadrant_is_extended (r));
}

int
p4est_quadrant_corner_level (const p4est_quadrant_t * q,
                             int zcorner, int level)
{
  p4est_qcoord_t      th, stepx, stepy;
  p4est_quadrant_t    quad, sibling;
  const int           zcorner_steps[4][2] =
    { {-1, -1,}, {1, -1}, {-1, 1}, {1, 1} };

  P4EST_ASSERT (p4est_quadrant_is_valid (q));
  P4EST_ASSERT (zcorner >= 0 && zcorner < 4);
  P4EST_ASSERT (level >= 0 && level <= P4EST_MAXLEVEL);

  P4EST_QUADRANT_INIT (&quad);
  P4EST_QUADRANT_INIT (&sibling);
  stepx = zcorner_steps[zcorner][0];
  stepy = zcorner_steps[zcorner][1];

  quad = *q;
  while (quad.level > level) {
    th = P4EST_LAST_OFFSET (quad.level);
    p4est_quadrant_sibling (&quad, &sibling, zcorner);
    if ((zcorner == 0 && sibling.x <= 0 && sibling.y <= 0) ||
        (zcorner == 1 && sibling.x >= th && sibling.y <= 0) ||
        (zcorner == 2 && sibling.x <= 0 && sibling.y >= th) ||
        (zcorner == 3 && sibling.x >= th && sibling.y >= th)) {
      return quad.level;
    }
    p4est_quadrant_parent (&quad, &quad);
    quad.x += stepx * P4EST_QUADRANT_LEN (quad.level);
    quad.y += stepy * P4EST_QUADRANT_LEN (quad.level);
    P4EST_ASSERT (p4est_quadrant_is_extended (&quad));
  }
  P4EST_ASSERT (quad.level == level);

  return level;
}

void
p4est_quadrant_corner (p4est_quadrant_t * q, int zcorner, int inside)
{
  p4est_qcoord_t      lshift, rshift;

  P4EST_ASSERT (q->level >= 0 && q->level <= P4EST_MAXLEVEL);

  lshift = (inside ? 0 : -P4EST_QUADRANT_LEN (q->level));
  rshift = (inside ? P4EST_LAST_OFFSET (q->level) : P4EST_ROOT_LEN);

  switch (zcorner) {
  case 0:
    q->x = q->y = lshift;
    break;
  case 1:
    q->x = rshift;
    q->y = lshift;
    break;
  case 2:
    q->x = lshift;
    q->y = rshift;
    break;
  case 3:
    q->x = q->y = rshift;
    break;
  default:
    SC_CHECK_NOT_REACHED ();
    break;
  }

  P4EST_ASSERT ((inside && p4est_quadrant_is_valid (q)) ||
                (!inside && p4est_quadrant_is_extended (q)));
}

void
p4est_quadrant_translate (p4est_quadrant_t * q, int face)
{
  P4EST_ASSERT (p4est_quadrant_is_extended (q));

  switch (face) {
  case 0:
    q->y += P4EST_ROOT_LEN;
    break;
  case 1:
    q->x -= P4EST_ROOT_LEN;
    break;
  case 2:
    q->y -= P4EST_ROOT_LEN;
    break;
  case 3:
    q->x += P4EST_ROOT_LEN;
    break;
  default:
    SC_CHECK_NOT_REACHED ();
    break;
  }

  P4EST_ASSERT (p4est_quadrant_is_extended (q));
}

int
p4est_node_transform (int node, int transform_type)
{
  int                 trans_node;

  P4EST_ASSERT (0 <= node && node < 4);

  switch (transform_type) {
  case 0:                      /* identity */
    trans_node = node;
    break;
  case 1:                      /* rotate -90 degrees */
    trans_node =
      p4est_corner_to_zorder[(p4est_corner_to_zorder[node] + 1) % 4];
    break;
  case 2:                      /* rotate 180 degrees */
    trans_node = 3 - node;
    break;
  case 3:                      /* rotate 90 degrees */
    trans_node =
      p4est_corner_to_zorder[(p4est_corner_to_zorder[node] + 3) % 4];
    break;
  case 4:                      /* mirror across 0 degree axis */
    switch (node) {
    case 0:
      trans_node = 2;
      break;
    case 1:
      trans_node = 3;
      break;
    case 2:
      trans_node = 0;
      break;
    case 3:
      trans_node = 1;
      break;
    default:
      SC_CHECK_NOT_REACHED ();
      break;
    }
    break;
  case 5:                      /* mirror across 45 degree axis */
    switch (node) {
    case 0:
      trans_node = 0;
      break;
    case 1:
      trans_node = 2;
      break;
    case 2:
      trans_node = 1;
      break;
    case 3:
      trans_node = 3;
      break;
    default:
      SC_CHECK_NOT_REACHED ();
      break;
    }
    break;
  case 6:                      /* mirror across 90 degree axis */
    switch (node) {
    case 0:
      trans_node = 1;
      break;
    case 1:
      trans_node = 0;
      break;
    case 2:
      trans_node = 3;
      break;
    case 3:
      trans_node = 2;
      break;
    default:
      SC_CHECK_NOT_REACHED ();
      break;
    }
    break;
  case 7:                      /* mirror across 135 degree axis */
    switch (node) {
    case 0:
      trans_node = 3;
      break;
    case 1:
      trans_node = 1;
      break;
    case 2:
      trans_node = 2;
      break;
    case 3:
      trans_node = 0;
      break;
    default:
      SC_CHECK_NOT_REACHED ();
      break;
    }
    break;
  default:
    SC_CHECK_NOT_REACHED ();
    break;
  }
  return trans_node;
}

void
p4est_quadrant_transform (const p4est_quadrant_t * q,
                          p4est_quadrant_t * r, int transform_type)
{
  p4est_qcoord_t      th;

  P4EST_ASSERT (q != r);
  P4EST_ASSERT (p4est_quadrant_is_extended (q));
  P4EST_ASSERT (0 <= transform_type && transform_type < 8);

  th = P4EST_LAST_OFFSET (q->level);

  switch (transform_type) {
  case 0:                      /* identity */
    r->x = q->x;
    r->y = q->y;
    break;
  case 1:                      /* rotate -90 degrees */
    r->x = th - q->y;
    r->y = q->x;
    break;
  case 2:                      /* rotate 180 degrees */
    r->x = th - q->x;
    r->y = th - q->y;
    break;
  case 3:                      /* rotate 90 degrees */
    r->x = q->y;
    r->y = th - q->x;
    break;
  case 4:                      /* mirror across 0 degree axis */
    r->x = q->x;
    r->y = th - q->y;
    break;
  case 5:                      /* mirror across 45 degree axis */
    r->x = q->y;
    r->y = q->x;
    break;
  case 6:                      /* mirror across 90 degree axis */
    r->x = th - q->x;
    r->y = q->y;
    break;
  case 7:                      /* mirror across 135 degree axis */
    r->x = th - q->y;
    r->y = th - q->x;
    break;
  default:
    SC_CHECK_NOT_REACHED ();
    break;
  }
  r->level = q->level;

  P4EST_ASSERT (p4est_quadrant_is_extended (r));
}

uint64_t
p4est_quadrant_linear_id (const p4est_quadrant_t * quadrant, int level)
{
  int                 i;
  uint64_t            x, y;
  uint64_t            id;

  P4EST_ASSERT (p4est_quadrant_is_extended (quadrant));
  P4EST_ASSERT (quadrant->level >= level && level >= 0);

  /* this preserves the high bits from negative numbers */
  x = quadrant->x >> (P4EST_MAXLEVEL - level);
  y = quadrant->y >> (P4EST_MAXLEVEL - level);

  id = 0;
  for (i = 0; i < level + (32 - P4EST_MAXLEVEL); ++i) {
    id |= ((x & ((uint64_t) 1 << i)) << i);
    id |= ((y & ((uint64_t) 1 << i)) << (i + 1));
  }

  return id;
}

void
p4est_quadrant_set_morton (p4est_quadrant_t * quadrant,
                           int level, uint64_t id)
{
  int                 i;

  P4EST_ASSERT (0 <= level && level <= P4EST_MAXLEVEL);
  if (level < P4EST_MAXLEVEL) {
    P4EST_ASSERT (id < ((uint64_t) 1 << 2 * (level + (32 - P4EST_MAXLEVEL))));
  }

  quadrant->level = (int8_t) level;
  quadrant->x = 0;
  quadrant->y = 0;

  /* this may set the sign bit to create negative numbers */
  for (i = 0; i < level + (32 - P4EST_MAXLEVEL); ++i) {
    quadrant->x |= (int32_t) ((id & (1ULL << (2 * i))) >> i);
    quadrant->y |= (int32_t) ((id & (1ULL << (2 * i + 1))) >> (i + 1));
  }

  quadrant->x <<= (P4EST_MAXLEVEL - level);
  quadrant->y <<= (P4EST_MAXLEVEL - level);

  P4EST_ASSERT (p4est_quadrant_is_extended (quadrant));
}

void
p4est_quadrant_init_data (p4est_t * p4est, p4est_topidx_t which_tree,
                          p4est_quadrant_t * quad, p4est_init_t init_fn)
{
  P4EST_ASSERT (p4est_quadrant_is_extended (quad));

  if (p4est->data_size > 0) {
    quad->p.user_data = sc_mempool_alloc (p4est->user_data_pool);
  }
  else {
    quad->p.user_data = NULL;
  }
  if (init_fn != NULL && p4est_quadrant_is_inside (quad)) {
    init_fn (p4est, which_tree, quad);
  }
}

void
p4est_quadrant_free_data (p4est_t * p4est, p4est_quadrant_t * quad)
{
  P4EST_ASSERT (p4est_quadrant_is_extended (quad));

  if (p4est->data_size > 0) {
    sc_mempool_free (p4est->user_data_pool, quad->p.user_data);
  }
  quad->p.user_data = NULL;
}

void
p4est_quadrant_print (int log_priority, const p4est_quadrant_t * q)
{
  P4EST_NORMAL_LOGF (log_priority,
                     "x 0x%x y 0x%x level %d\n", q->x, q->y, q->level);
}

unsigned
p4est_quadrant_checksum (sc_array_t * quadrants,
                         sc_array_t * checkarray, int first_quadrant)
{
  int                 own_check;
  int                 k, qcount;
  unsigned            crc;
  uint32_t           *check;
  p4est_quadrant_t   *q;

  qcount = quadrants->elem_count;

  P4EST_ASSERT (quadrants->elem_size == sizeof (p4est_quadrant_t));
  P4EST_ASSERT (0 <= first_quadrant && first_quadrant <= qcount);

  if (checkarray == NULL) {
    checkarray = sc_array_new (4);
    own_check = 1;
  }
  else {
    P4EST_ASSERT (checkarray->elem_size == 4);
    own_check = 0;
  }

  sc_array_resize (checkarray, (qcount - first_quadrant) * 3);
  for (k = first_quadrant; k < qcount; ++k) {
    q = sc_array_index (quadrants, k);
    P4EST_ASSERT (p4est_quadrant_is_extended (q));
    check = sc_array_index (checkarray, (k - first_quadrant) * 3);
    check[0] = htonl ((uint32_t) q->x);
    check[1] = htonl ((uint32_t) q->y);
    check[2] = htonl ((uint32_t) q->level);
  }
  crc = sc_array_checksum (checkarray, 0);

  if (own_check) {
    sc_array_destroy (checkarray);
  }

  return crc;
}

int
p4est_tree_is_sorted (p4est_tree_t * tree)
{
  int                 i;
  p4est_quadrant_t   *q1, *q2;
  sc_array_t         *tquadrants = &tree->quadrants;

  if (tquadrants->elem_count <= 1) {
    return 1;
  }

  q1 = sc_array_index (tquadrants, 0);
  for (i = 1; i < tquadrants->elem_count; ++i) {
    q2 = sc_array_index (tquadrants, i);
    if (p4est_quadrant_compare (q1, q2) >= 0) {
      return 0;
    }
    q1 = q2;
  }

  return 1;
}

int
p4est_tree_is_linear (p4est_tree_t * tree)
{
  int                 i;
  p4est_quadrant_t   *q1, *q2;
  sc_array_t         *tquadrants = &tree->quadrants;

  if (tquadrants->elem_count <= 1) {
    return 1;
  }

  q1 = sc_array_index (tquadrants, 0);
  for (i = 1; i < tquadrants->elem_count; ++i) {
    q2 = sc_array_index (tquadrants, i);
    if (p4est_quadrant_compare (q1, q2) >= 0) {
      return 0;
    }
    if (p4est_quadrant_is_ancestor (q1, q2)) {
      return 0;
    }
    q1 = q2;
  }

  return 1;
}

int
p4est_tree_is_almost_sorted (p4est_tree_t * tree, int check_linearity)
{
  int                 i;
  int                 face_contact1;
  int                 face_contact2;
  p4est_quadrant_t   *q1, *q2;
  sc_array_t         *tquadrants = &tree->quadrants;

  if (tquadrants->elem_count <= 1) {
    return 1;
  }

  q1 = sc_array_index (tquadrants, 0);
  face_contact1 = 0;
  face_contact1 |= ((q1->y < 0) ? 0x01 : 0);
  face_contact1 |= ((q1->x >= P4EST_ROOT_LEN) ? 0x02 : 0);
  face_contact1 |= ((q1->y >= P4EST_ROOT_LEN) ? 0x04 : 0);
  face_contact1 |= ((q1->x < 0) ? 0x08 : 0);
  for (i = 1; i < tquadrants->elem_count; ++i) {
    q2 = sc_array_index (tquadrants, i);
    face_contact2 = 0;
    face_contact2 |= ((q2->y < 0) ? 0x01 : 0);
    face_contact2 |= ((q2->x >= P4EST_ROOT_LEN) ? 0x02 : 0);
    face_contact2 |= ((q2->y >= P4EST_ROOT_LEN) ? 0x04 : 0);
    face_contact2 |= ((q2->x < 0) ? 0x08 : 0);

    if ((face_contact1 & 0x05) && (face_contact1 & 0x0a) &&
        face_contact1 == face_contact2) {
      /* both quadrants are outside the same corner and may overlap */
    }
    else {
      if (p4est_quadrant_compare (q1, q2) >= 0) {
        return 0;
      }
      if (check_linearity && p4est_quadrant_is_ancestor (q1, q2)) {
        return 0;
      }
    }
    q1 = q2;
    face_contact1 = face_contact2;
  }

  return 1;
}

int
p4est_tree_is_complete (p4est_tree_t * tree)
{
  int                 i;
  p4est_quadrant_t   *q1, *q2;
  sc_array_t         *tquadrants = &tree->quadrants;

  if (tquadrants->elem_count <= 1) {
    return 1;
  }

  q1 = sc_array_index (tquadrants, 0);
  for (i = 1; i < tquadrants->elem_count; ++i) {
    q2 = sc_array_index (tquadrants, i);
    if (!p4est_quadrant_is_next (q1, q2)) {
      return 0;
    }
    q1 = q2;
  }

  return 1;
}

void
p4est_tree_print (int log_priority, p4est_tree_t * tree)
{
  int                 j, l, childid, comp;
  char                buffer[BUFSIZ];
  p4est_quadrant_t   *q1, *q2;
  sc_array_t         *tquadrants = &tree->quadrants;

  q1 = NULL;
  for (j = 0; j < tquadrants->elem_count; ++j) {
    q2 = sc_array_index (tquadrants, j);
    childid = p4est_quadrant_child_id (q2);
    l = snprintf (buffer, BUFSIZ, "0x%llx 0x%llx %d",
                  (long long) q2->x, (long long) q2->y, q2->level);
    if (j > 0) {
      comp = p4est_quadrant_compare (q1, q2);
      if (comp > 0) {
        l += snprintf (buffer + l, BUFSIZ - l, " R");
      }
      else if (comp == 0) {
        l += snprintf (buffer + l, BUFSIZ - l, " I");
      }
      else {
        if (p4est_quadrant_is_sibling (q1, q2)) {
          l += snprintf (buffer + l, BUFSIZ - l, " S%d", childid);
        }
        else if (p4est_quadrant_is_parent (q1, q2)) {
          l += snprintf (buffer + l, BUFSIZ - l, " C%d", childid);
        }
        else if (p4est_quadrant_is_ancestor (q1, q2)) {
          l += snprintf (buffer + l, BUFSIZ - l, " D");
        }
        else if (p4est_quadrant_is_next (q1, q2)) {
          l += snprintf (buffer + l, BUFSIZ - l, " N%d", childid);
        }
        else {
          l += snprintf (buffer + l, BUFSIZ - l, " q%d", childid);
        }
      }
    }
    else {
      l += snprintf (buffer + l, BUFSIZ - l, " F%d", childid);
    }
    l += snprintf (buffer + l, BUFSIZ - l, "\n");
    P4EST_NORMAL_LOG (log_priority, buffer);
    q1 = q2;
  }
}

int
p4est_is_valid (p4est_t * p4est)
{
  const int           num_procs = p4est->mpisize;
  const int           rank = p4est->mpirank;
  const p4est_topidx_t first_tree = p4est->first_local_tree;
  const p4est_topidx_t last_tree = p4est->last_local_tree;
  int                 i, maxlevel;
  size_t              js;
  p4est_topidx_t      next_tree;
  p4est_locidx_t      nquadrants, lquadrants, perlevel;
  p4est_quadrant_t   *q;
  p4est_quadrant_t    mylow, nextlow, s;
  p4est_tree_t       *tree;

  P4EST_QUADRANT_INIT (&mylow);
  P4EST_QUADRANT_INIT (&nextlow);
  P4EST_QUADRANT_INIT (&s);

  /* check last item of global partition */
  P4EST_ASSERT (p4est->global_first_position[num_procs].which_tree ==
                p4est->connectivity->num_trees &&
                p4est->global_first_position[num_procs].x == 0 &&
                p4est->global_first_position[num_procs].y == 0);
  P4EST_ASSERT (p4est->connectivity->num_trees ==
                (p4est_topidx_t) p4est->trees->elem_count);

  /* check first tree in global partition */
  if (first_tree < 0) {
    if (!(first_tree == -1 && last_tree == -2)) {
      P4EST_INFO ("p4est invalid empty tree range A");
      return 0;
    }
  }
  else {
    if (p4est->global_first_position[rank].which_tree != first_tree) {
      P4EST_INFO ("p4est invalid first tree\n");
      return 0;
    }
    mylow.x = p4est->global_first_position[rank].x;
    mylow.y = p4est->global_first_position[rank].y;
    mylow.level = P4EST_MAXLEVEL;
    tree = sc_array_index (p4est->trees, first_tree);
    if (tree->quadrants.elem_count > 0) {
      q = sc_array_index (&tree->quadrants, 0);
      if (q->x != mylow.x || q->y != mylow.y) {
        P4EST_INFO ("p4est invalid low quadrant\n");
        return 0;
      }
    }
  }

  /* check last tree in global partition */
  if (last_tree < 0) {
    if (!(first_tree == -1 && last_tree == -2)) {
      P4EST_INFO ("p4est invalid empty tree range B");
      return 0;
    }
  }
  else {
    next_tree = p4est->global_first_position[rank + 1].which_tree;
    if (next_tree != last_tree && next_tree != last_tree + 1) {
      P4EST_INFO ("p4est invalid last tree\n");
      return 0;
    }
    nextlow.x = p4est->global_first_position[rank + 1].x;
    nextlow.y = p4est->global_first_position[rank + 1].y;
    nextlow.level = P4EST_MAXLEVEL;
    tree = sc_array_index (p4est->trees, last_tree);
    if (tree->quadrants.elem_count > 0) {
      q = sc_array_index (&tree->quadrants, tree->quadrants.elem_count - 1);
      if (next_tree == last_tree) {
        if (!p4est_quadrant_is_next (q, &nextlow)) {
          P4EST_INFO ("p4est invalid next quadrant\n");
          return 0;
        }
      }
      else {
        p4est_quadrant_last_descendent (q, &s, P4EST_MAXLEVEL);
        if (s.x + 1 != P4EST_ROOT_LEN || s.y + 1 != P4EST_ROOT_LEN) {
          P4EST_INFO ("p4est invalid last quadrant\n");
          return 0;
        }
      }
    }
  }

  /* check individual trees */
  lquadrants = 0;
  for (js = 0; js < p4est->trees->elem_count; ++js) {
    tree = sc_array_index (p4est->trees, js);
    if (!p4est_tree_is_complete (tree)) {
      P4EST_INFO ("p4est invalid not complete\n");
      return 0;
    }
    if ((js < p4est->first_local_tree || js > p4est->last_local_tree) &&
        tree->quadrants.elem_count > 0) {
      P4EST_INFO ("p4est invalid outside count\n");
      return 0;
    }

    maxlevel = 0;
    nquadrants = 0;
    for (i = 0; i <= P4EST_MAXLEVEL; ++i) {
      perlevel = tree->quadrants_per_level[i];

      P4EST_ASSERT (perlevel >= 0);
      nquadrants += perlevel;
      if (perlevel > 0) {
        maxlevel = i;
      }
    }
    lquadrants += nquadrants;

    if (maxlevel != tree->maxlevel) {
      P4EST_INFO ("p4est invalid wrong maxlevel\n");
      return 0;
    }
    if (nquadrants != tree->quadrants.elem_count) {
      P4EST_INFO ("p4est invalid tree quadrant count\n");
      return 0;
    }
  }

  if (lquadrants != p4est->local_num_quadrants) {
    P4EST_INFO ("p4est invalid local quadrant count\n");
    return 0;
  }

  return 1;
}

/* here come the heavyweight algorithms */

int
p4est_find_lower_bound (sc_array_t * array,
                        const p4est_quadrant_t * q, int guess)
{
  int                 count, comp;
  int                 quad_low, quad_high;
  p4est_quadrant_t   *cur;

  count = array->elem_count;
  quad_low = 0;
  quad_high = count - 1;

  for (;;) {
    P4EST_ASSERT (quad_low <= quad_high);
    P4EST_ASSERT (0 <= quad_low && quad_low < count);
    P4EST_ASSERT (0 <= quad_high && quad_high < count);
    P4EST_ASSERT (quad_low <= guess && guess <= quad_high);

    /* compare two quadrants */
    cur = sc_array_index (array, guess);
    comp = p4est_quadrant_compare (q, cur);

    /* check if guess is higher or equal q and there's room below it */
    if (comp <= 0 && (guess > 0 && p4est_quadrant_compare (q, cur - 1) <= 0)) {
      quad_high = guess - 1;
      guess = (quad_low + quad_high + 1) / 2;
      continue;
    }

    /* check if guess is lower than q */
    if (comp > 0) {
      quad_low = guess + 1;
      if (quad_low > quad_high) {
        return -1;
      }
      guess = (quad_low + quad_high) / 2;
      continue;
    }

    /* otherwise guess is the correct quadrant */
    break;
  }

  return guess;
}

int
p4est_find_higher_bound (sc_array_t * array,
                         const p4est_quadrant_t * q, int guess)
{
  int                 count, comp;
  int                 quad_low, quad_high;
  p4est_quadrant_t   *cur;

  count = array->elem_count;
  quad_low = 0;
  quad_high = count - 1;

  for (;;) {
    P4EST_ASSERT (quad_low <= quad_high);
    P4EST_ASSERT (0 <= quad_low && quad_low < count);
    P4EST_ASSERT (0 <= quad_high && quad_high < count);
    P4EST_ASSERT (quad_low <= guess && guess <= quad_high);

    /* compare two quadrants */
    cur = sc_array_index (array, guess);
    comp = p4est_quadrant_compare (cur, q);

    /* check if guess is lower or equal q and there's room above it */
    if (comp <= 0 &&
        (guess < count - 1 && p4est_quadrant_compare (cur + 1, q) <= 0)) {
      quad_low = guess + 1;
      guess = (quad_low + quad_high) / 2;
      continue;
    }

    /* check if guess is higher than q */
    if (comp > 0) {
      quad_high = guess - 1;
      if (quad_high < quad_low) {
        return -1;
      }
      guess = (quad_low + quad_high + 1) / 2;
      continue;
    }

    /* otherwise guess is the correct quadrant */
    break;
  }

  return guess;
}

void
p4est_tree_compute_overlap (p4est_t * p4est, p4est_topidx_t qtree,
                            sc_array_t * in, sc_array_t * out)
{
  int                 i, j, guess;
  int                 k, l, which;
  int                 ctree;
  int                 treecount, incount, outcount;
  int                 first_index, last_index;
  int                 inter_tree, transform, outface[4];
  int                 face, corner, zcorner = -1, level;
  int32_t             ntree;
  int32_t             qh;
  sc_array_t          corner_info;
  p4est_quadrant_t    treefd, treeld;
  p4est_quadrant_t    fd, ld, tempq, ins[9], cq;
  p4est_quadrant_t   *tq, *s;
  p4est_quadrant_t   *inq, *outq;
  p4est_tree_t       *tree;
  p4est_connectivity_t *conn = p4est->connectivity;
  p4est_corner_info_t *ci;
  sc_array_t         *tquadrants;

  tree = sc_array_index (p4est->trees, qtree);
  P4EST_ASSERT (p4est_tree_is_complete (tree));
  tquadrants = &tree->quadrants;

  P4EST_QUADRANT_INIT (&treefd);
  P4EST_QUADRANT_INIT (&treeld);
  P4EST_QUADRANT_INIT (&fd);
  P4EST_QUADRANT_INIT (&ld);
  P4EST_QUADRANT_INIT (&tempq);
  P4EST_QUADRANT_INIT (&cq);
  for (which = 0; which < 9; ++which) {
    P4EST_QUADRANT_INIT (&ins[which]);
  }
  sc_array_init (&corner_info, sizeof (p4est_corner_info_t));

  /* assign some numbers */
  treecount = tquadrants->elem_count;
  P4EST_ASSERT (treecount > 0);
  incount = in->elem_count;
  outcount = out->elem_count;

  /* return if there is nothing to do */
  if (treecount == 0 || incount == 0) {
    return;
  }

  /* compute first and last descendants in the tree */
  tq = sc_array_index (tquadrants, 0);
  p4est_quadrant_first_descendent (tq, &treefd, P4EST_MAXLEVEL);
  tq = sc_array_index (tquadrants, treecount - 1);
  p4est_quadrant_last_descendent (tq, &treeld, P4EST_MAXLEVEL);

  /* loop over input list of quadrants */
  for (i = 0; i < incount; ++i) {
    inq = sc_array_index (in, i);
    if (inq->p.piggy.which_tree != qtree) {
      continue;
    }
    inter_tree = 0;
    ntree = qtree;
    face = corner = -1;
    transform = -1;
    if (!p4est_quadrant_is_inside (inq)) {
      /* this quadrant comes from a different tree */
      P4EST_ASSERT (p4est_quadrant_is_extended (inq));
      inter_tree = 1;
      outface[0] = (inq->y < 0);
      outface[1] = (inq->x >= P4EST_ROOT_LEN);
      outface[2] = (inq->y >= P4EST_ROOT_LEN);
      outface[3] = (inq->x < 0);
      if ((outface[0] || outface[2]) && (outface[1] || outface[3])) {
        /* this quadrant is a corner neighbor */
        for (corner = 0; corner < 4; ++corner) {
          if (outface[(corner + 3) % 4] && outface[corner]) {
            break;
          }
        }
        p4est_find_corner_info (conn, qtree, corner, &corner_info);

        /* construct highest corner quadrant */
        cq.level = P4EST_MAXLEVEL;
        zcorner = p4est_corner_to_zorder[corner];
        p4est_quadrant_corner (&cq, zcorner, 1);
      }
      else {
        /* this quadrant is a face neighbor */
        for (face = 0; face < 4; ++face) {
          if (outface[face]) {
            break;
          }
        }
        P4EST_ASSERT (face < 4);
        ntree = conn->tree_to_tree[4 * qtree + face];
        transform = p4est_find_face_transform (conn, qtree, face);
      }
    }
    qh = P4EST_QUADRANT_LEN (inq->level);

    /* loop over the insulation layer of inq */
    for (k = 0; k < 3; ++k) {
      for (l = 0; l < 3; ++l) {
        which = k * 3 + l;      /* 0..8 */

        /* exclude myself from the queries */
        if (which == 4) {
          continue;
        }
        s = &ins[which];
        *s = *inq;
        s->x += (l - 1) * qh;
        s->y += (k - 1) * qh;
        if ((s->x < 0 || s->x >= P4EST_ROOT_LEN) ||
            (s->y < 0 || s->y >= P4EST_ROOT_LEN)) {
          /* this quadrant is outside this tree, no overlap */
          continue;
        }
        p4est_quadrant_first_descendent (s, &fd, P4EST_MAXLEVEL);
        p4est_quadrant_last_descendent (s, &ld, P4EST_MAXLEVEL);

        /* skip this insulation quadrant if there is no overlap */
        if (p4est_quadrant_compare (&ld, &treefd) < 0 ||
            p4est_quadrant_compare (&treeld, &fd) < 0) {
          continue;
        }

        /* find first quadrant in tree that fits between fd and ld */
        guess = treecount / 2;
        if (p4est_quadrant_compare (&fd, &treefd) <= 0) {
          /* the first tree quadrant is contained in insulation quadrant */
          first_index = 0;
        }
        else {
          /* do a binary search for the lowest tree quadrant >= s */
          first_index = p4est_find_lower_bound (tquadrants, s, guess);
          if (first_index < 0) {
            continue;
          }
          guess = first_index;
        }

        /* find last quadrant in tree that fits between fd and ld */
        if (p4est_quadrant_compare (&treeld, &ld) <= 0) {
          /* the last tree quadrant is contained in insulation layer */
          last_index = treecount - 1;
        }
        else {
          /* do a binary search for the highest tree quadrant <= ld */
          last_index = p4est_find_higher_bound (tquadrants, &ld, guess);
          if (last_index < 0) {
            printf ("Last index < 0\n");
            continue;
          }
        }

        /* skip if no overlap of sufficient level difference is found */
        if (first_index > last_index) {
          continue;
        }

        /* copy relevant quadrants into out */
        if (inter_tree && corner >= 0) {
          /* across the corner, find smallest corner quadrant to be sent */
          level = 0;
          for (j = first_index; j <= last_index; ++j) {
            tq = sc_array_index (tquadrants, j);
            if (tq->level <= level) {
              continue;
            }
            level = p4est_quadrant_corner_level (tq, zcorner, level);
          }
          zcorner = -1;         /* will be recycled below */

          /* send this small corner to all neighbor corner trees */
          for (ctree = 0; ctree < corner_info.elem_count; ++ctree) {
            ci = sc_array_index (&corner_info, ctree);

            sc_array_resize (out, outcount + 1);
            outq = sc_array_index (out, outcount);
            outq->level = (int8_t) level;
            zcorner = p4est_corner_to_zorder[ci->ncorner];
            p4est_quadrant_corner (outq, zcorner, 0);
            outq->p.piggy.which_tree = ci->ntree;
            ++outcount;
          }
        }
        else {
          /* across face or intra-tree, find quadrants that are small enough */
          for (j = first_index; j <= last_index; ++j) {
            tq = sc_array_index (tquadrants, j);
            if (tq->level > inq->level + 1) {
              sc_array_resize (out, outcount + 1);
              outq = sc_array_index (out, outcount);
              if (inter_tree) {
                tempq = *tq;
                p4est_quadrant_translate (&tempq, face);
                p4est_quadrant_transform (&tempq, outq, transform);
              }
              else {
                *outq = *tq;
              }
              outq->p.piggy.which_tree = ntree;
              ++outcount;
            }
          }
        }
      }
    }
  }

  sc_array_reset (&corner_info);
}

void
p4est_tree_uniqify_overlap (sc_array_t * not, sc_array_t * out)
{
  int                 i, j;
  int                 outcount, dupcount, notcount;
  p4est_quadrant_t   *inq, *outq, *tq;

  outcount = out->elem_count;
  if (outcount == 0) {
    return;
  }

  /* sort array and remove duplicates */
  sc_array_sort (out, p4est_quadrant_compare);
  dupcount = notcount = 0;
  i = 0;                        /* read counter */
  j = 0;                        /* write counter */
  inq = sc_array_index (out, i);
  while (i < outcount) {
    tq = ((i < outcount - 1) ? sc_array_index (out, i + 1) : NULL);
    if (i < outcount - 1 && p4est_quadrant_is_equal (inq, tq)) {
      ++dupcount;
      ++i;
    }
    else if (sc_array_bsearch (not, inq, p4est_quadrant_compare_piggy) != -1) {
      ++notcount;
      ++i;
    }
    else {
      if (i > j) {
        outq = sc_array_index (out, j);
        *outq = *inq;
      }
      ++i;
      ++j;
    }
    inq = tq;
  }
  P4EST_ASSERT (i == outcount);
  P4EST_ASSERT (j + dupcount + notcount == outcount);
  sc_array_resize (out, j);
}

void
p4est_complete_region (p4est_t * p4est,
                       const p4est_quadrant_t * q1,
                       int include_q1,
                       const p4est_quadrant_t * q2,
                       int include_q2,
                       p4est_tree_t * tree,
                       p4est_topidx_t which_tree, p4est_init_t init_fn)
{
  p4est_tree_t       *R;
  sc_list_t          *W;

  p4est_quadrant_t    a = *q1;
  p4est_quadrant_t    b = *q2;

  p4est_quadrant_t    Afinest;
  p4est_quadrant_t   *c0, *c1, *c2, *c3;

  sc_array_t         *quadrants;
  sc_mempool_t       *quadrant_pool = p4est->quadrant_pool;

  p4est_quadrant_t   *w;
  p4est_quadrant_t   *r;

  int                 comp;
  int                 quadrant_pool_size;
  int                 data_pool_size = -1;
  int                 level, maxlevel = 0;
  int32_t            *quadrants_per_level;
  int32_t             num_quadrants = 0;

  P4EST_QUADRANT_INIT (&Afinest);

  W = sc_list_new (NULL);
  R = tree;

  /* needed for sanity check */
  quadrant_pool_size = p4est->quadrant_pool->elem_count;
  if (p4est->user_data_pool != NULL) {
    data_pool_size = p4est->user_data_pool->elem_count;
  }

  quadrants = &R->quadrants;
  quadrants_per_level = R->quadrants_per_level;

  /* Assert that we have an empty tree */
  P4EST_ASSERT (quadrants->elem_count == 0);

  comp = p4est_quadrant_compare (&a, &b);
  /* Assert that a<b */
  P4EST_ASSERT (comp < 0);

  /* R <- R + a */
  if (include_q1) {
    sc_array_resize (quadrants, 1);
    r = sc_array_index (quadrants, 0);
    *r = a;
    maxlevel = SC_MAX (a.level, maxlevel);
    ++quadrants_per_level[a.level];
    ++num_quadrants;
  }

  if (comp < 0) {
    /* W <- C(A_{finest}(a,b)) */
    p4est_nearest_common_ancestor (&a, &b, &Afinest);

    c0 = sc_mempool_alloc (quadrant_pool);
    c1 = sc_mempool_alloc (quadrant_pool);
    c2 = sc_mempool_alloc (quadrant_pool);
    c3 = sc_mempool_alloc (quadrant_pool);

    p4est_quadrant_children (&Afinest, c0, c1, c2, c3);

    sc_list_append (W, c0);
    sc_list_append (W, c1);
    sc_list_append (W, c2);
    sc_list_append (W, c3);

    /* for each w in W */
    while (W->elem_count > 0) {
      w = sc_list_pop (W);
      level = w->level;

      /* if (a < w < b) and (w not in {A(b)}) */
      if (((p4est_quadrant_compare (&a, w) < 0) &&
           (p4est_quadrant_compare (w, &b) < 0)
          ) && !p4est_quadrant_is_ancestor (w, &b)
        ) {
        /* R <- R + w */
        sc_array_resize (quadrants, num_quadrants + 1);
        r = sc_array_index (quadrants, num_quadrants);
        *r = *w;
        p4est_quadrant_init_data (p4est, which_tree, r, init_fn);
        maxlevel = SC_MAX (level, maxlevel);
        ++quadrants_per_level[level];
        ++num_quadrants;
      }
      /* else if (w in {{A(a)}, {A(b)}}) */
      else if (p4est_quadrant_is_ancestor (w, &a)
               || p4est_quadrant_is_ancestor (w, &b)) {
        /* W <- W + C(w) */
        c0 = sc_mempool_alloc (quadrant_pool);
        c1 = sc_mempool_alloc (quadrant_pool);
        c2 = sc_mempool_alloc (quadrant_pool);
        c3 = sc_mempool_alloc (quadrant_pool);

        p4est_quadrant_children (w, c0, c1, c2, c3);

        sc_list_prepend (W, c3);
        sc_list_prepend (W, c2);
        sc_list_prepend (W, c1);
        sc_list_prepend (W, c0);
      }

      /* W <- W - w */
      sc_mempool_free (quadrant_pool, w);
    }                           /* end for */

    /* R <- R + b */
    if (include_q2) {
      sc_array_resize (quadrants, num_quadrants + 1);
      r = sc_array_index (quadrants, num_quadrants);
      *r = b;
      maxlevel = SC_MAX (b.level, maxlevel);
      ++quadrants_per_level[b.level];
      ++num_quadrants;
    }
  }

  R->maxlevel = (int8_t) maxlevel;

  P4EST_ASSERT (W->first == NULL && W->last == NULL);
  sc_list_destroy (W);

  P4EST_ASSERT (p4est_tree_is_complete (R));
  P4EST_ASSERT (quadrant_pool_size == p4est->quadrant_pool->elem_count);
  P4EST_ASSERT (num_quadrants == quadrants->elem_count);
  if (p4est->user_data_pool != NULL) {
    P4EST_ASSERT (data_pool_size + quadrants->elem_count ==
                  p4est->user_data_pool->elem_count + (include_q1 ? 1 : 0)
                  + (include_q2 ? 1 : 0));
  }
}

/** Internal function to realize local completion / balancing.
 * \param [in] balance  can be 0: no balancing
 *                             1: balance across edges
 *                             2: balance across edges and corners
 */
static void
p4est_complete_or_balance (p4est_t * p4est, p4est_tree_t * tree, int balance,
                           p4est_topidx_t which_tree, p4est_init_t init_fn)
{
  int                 i, j;
  int                 incount, curcount, ocount;
  int                 comp, lookup, inserted, isfamily, isoutroot;
  int                 quadrant_pool_size;
  int                 data_pool_size = -1;
  int                 count_outside_root, count_outside_tree;
  int                 count_already_inlist, count_already_outlist;
  int                 first_inside, last_inside;
  int                 qid, sid, pid, bbound;
  int                *key, *parent_key;
  int                 outface[4];
  int                 l, inmaxl;
  void               *vlookup;
  ssize_t             srindex;
  p4est_qcoord_t      ph;
  p4est_quadrant_t   *family[4];
  p4est_quadrant_t   *q;
  p4est_quadrant_t   *qalloc, *qlookup, **qpointer;
  p4est_quadrant_t    ld, tree_first, tree_last, parent;
  sc_array_t         *inlist, *olist;
  sc_mempool_t       *list_alloc, *qpool;
  sc_hash_t          *hash[P4EST_MAXLEVEL + 1];
  sc_array_t          outlist[P4EST_MAXLEVEL + 1];

  P4EST_ASSERT (p4est_tree_is_almost_sorted (tree, 1));

  P4EST_QUADRANT_INIT (&ld);
  P4EST_QUADRANT_INIT (&tree_first);
  P4EST_QUADRANT_INIT (&tree_last);
  P4EST_QUADRANT_INIT (&parent);

  /*
   * Algorithm works with these data structures
   * inlist  --  sorted list of input quadrants
   * hash    --  hash table to hold additional quadrants not in inlist
   *             this is filled bottom-up to ensure balance condition
   * outlist --  filled simultaneously with hash, holding pointers
   *             don't rely on addresses of elements, it is resized
   * In the end, the elements of hash are appended to inlist
   * and inlist is sorted and linearized. This can be optimized later.
   */

  /* assign some shortcut variables */
  bbound = ((balance == 0) ? 5 : 8);
  inlist = &tree->quadrants;
  incount = inlist->elem_count;
  inmaxl = tree->maxlevel;
  qpool = p4est->quadrant_pool;
  key = &comp;                  /* unique user_data pointer */
  parent_key = &lookup;

  /* needed for sanity check */
  quadrant_pool_size = qpool->elem_count;
  if (p4est->user_data_pool != NULL) {
    data_pool_size = p4est->user_data_pool->elem_count;
  }

  /* if tree is empty or a single block, there is nothing to do */
  if (incount <= 1) {
    return;
  }

  /* determine the first and last small quadrants contained in the tree */
  first_inside = incount;
  q = NULL;
  for (i = 0; i < incount; ++i) {
    q = sc_array_index (inlist, i);
    if (p4est_quadrant_is_inside (q)) {
      first_inside = i;
      p4est_quadrant_first_descendent (q, &tree_first, inmaxl);
      break;
    }
  }
  if (i == incount) {
    /* only extended quadrants in the tree, there is nothing to do */
    return;
  }
  last_inside = incount - 1;
  p4est_quadrant_last_descendent (q, &tree_last, inmaxl);
  for (i = first_inside + 1; i < incount; ++i) {
    q = sc_array_index (inlist, i);
    if (!p4est_quadrant_is_inside (q)) {
      last_inside = i - 1;
      break;
    }
    p4est_quadrant_last_descendent (q, &ld, inmaxl);
    comp = p4est_quadrant_compare (&tree_last, &ld);
    if (comp < 0) {
      tree_last = ld;
    }
  }
  P4EST_ASSERT (first_inside <= last_inside && last_inside < incount);
  P4EST_ASSERT (p4est_quadrant_is_valid (&tree_first));
  P4EST_ASSERT (p4est_quadrant_is_valid (&tree_last));

  /* initialize some counters */
  count_outside_root = count_outside_tree = 0;
  count_already_inlist = count_already_outlist = 0;

  /* initialize temporary storage */
  list_alloc = sc_mempool_new (sizeof (sc_link_t));
  for (l = 0; l <= inmaxl; ++l) {
    hash[l] = sc_hash_new (p4est_quadrant_hash, p4est_quadrant_is_equal,
                           list_alloc);
    sc_array_init (&outlist[l], sizeof (p4est_quadrant_t *));
  }
  for (l = inmaxl + 1; l <= P4EST_MAXLEVEL; ++l) {
    hash[l] = NULL;
  }

  /* walk through the input tree bottom-up */
  ph = 0;
  pid = -1;
  qalloc = sc_mempool_alloc (qpool);
  qalloc->p.user_data = key;
  for (l = inmaxl; l > 0; --l) {
    ocount = outlist[l].elem_count;     /* fix ocount here, it is growing */
    for (i = 0; i < incount + ocount; ++i) {
      isfamily = 0;
      if (i < incount) {
        q = sc_array_index (inlist, i);
        if (q->level != l) {
          continue;
        }
        /* this is an optimization to catch adjacent siblings */
        if (i + 4 <= incount) {
          family[0] = q;
          for (j = 1; j < 4; ++j) {
            family[j] = sc_array_index (inlist, i + j);
          }
          if (p4est_quadrant_is_family (family[0], family[1],
                                        family[2], family[3])) {
            isfamily = 1;
            i += 3;             /* skip siblings */
          }
        }
      }
      else {
        qpointer = sc_array_index (&outlist[l], i - incount);
        q = *qpointer;
        P4EST_ASSERT (q->level == l);
      }
      P4EST_ASSERT (p4est_quadrant_is_extended (q));
      isoutroot = !p4est_quadrant_is_inside (q);

      /*
       * check for q and its siblings,
       * then for q's parent and parent's indirect relevant neighbors
       * sid == 0..3  siblings including q
       *        4     parent of q
       *        5..7  relevant indirect neighbors of parent
       *              one of them is omitted if corner balance is off
       *
       * if q is inside the tree, include all of the above.
       * if q is outside the tree, include only its parent and the neighbors.
       */
      qid = p4est_quadrant_child_id (q);        /* 0 <= qid < 4 */
      for (sid = 0; sid < bbound; ++sid) {
        /* stage 1: determine candidate qalloc */
        if (sid < 4) {
          if (qid == sid || isfamily) {
            /* q (or its family) is included in inlist */
            continue;
          }
          if (isoutroot) {
            /* q is outside the tree */
            continue;
          }
          p4est_quadrant_sibling (q, qalloc, sid);
        }
        else if (sid == 4) {
          /* compute the parent */
          p4est_quadrant_parent (q, qalloc);
          if (bbound > 5) {
            parent = *qalloc;   /* copy parent for cases 5..7 */
            ph = P4EST_QUADRANT_LEN (parent.level);     /* its size */
            pid = p4est_quadrant_child_id (&parent);    /* and position */
          }
        }
        else {
          /* determine the 3 parent's relevant indirect neighbors */
          P4EST_ASSERT (sid >= 5 && sid < 8);
          if (balance < 2 && sid - 5 == corners_omitted[pid]) {
            /* this quadrant would only be needed for corner balance */
            continue;
          }
          qalloc->x = parent.x + indirect_neighbors[pid][sid - 5][0] * ph;
          qalloc->y = parent.y + indirect_neighbors[pid][sid - 5][1] * ph;
          qalloc->level = parent.level;
          outface[0] = (qalloc->y < 0);
          outface[1] = (qalloc->x >= P4EST_ROOT_LEN);
          outface[2] = (qalloc->y >= P4EST_ROOT_LEN);
          outface[3] = (qalloc->x < 0);
          if (!isoutroot) {
            if (outface[0] || outface[1] || outface[2] || outface[3]) {
              /* q is inside and this quadrant is outside the root */
              ++count_outside_root;
              continue;
            }
          }
          else {
            if ((outface[0] || outface[2]) && (outface[1] || outface[3])) {
              /* quadrant is outside and across the corner */
              ++count_outside_root;
              continue;
            }
          }
        }
        /*
           printf ("Candidate level %d qxy 0x%x 0x%x at sid %d\n",
           qalloc->level, qalloc->x, qalloc->y, sid);
         */

        /* stage 2: include qalloc if necessary */
        if (p4est_quadrant_is_inside (qalloc)) {
          p4est_quadrant_last_descendent (qalloc, &ld, inmaxl);
          if ((p4est_quadrant_compare (&tree_first, qalloc) > 0 &&
               (qalloc->x != tree_first.x || qalloc->y != tree_first.y)) ||
              p4est_quadrant_compare (&ld, &tree_last) > 0) {
            /* qalloc is outside the tree */
            ++count_outside_tree;
            continue;
          }
        }
        lookup = sc_hash_lookup (hash[qalloc->level], qalloc, &vlookup);
        if (lookup) {
          /* qalloc is already included in output list, this catches most */
          ++count_already_outlist;
          qlookup = vlookup;
          if (sid == 4 && qlookup->p.user_data == parent_key) {
            break;              /* this parent has been triggered before */
          }
          continue;
        }
        srindex = sc_array_bsearch (inlist, qalloc, p4est_quadrant_compare);
        if (srindex != -1) {
          /* qalloc is included in inlist, this is more expensive to test */
          ++count_already_inlist;
          continue;
        }
        /* insert qalloc into the output list as well */
        if (sid == 4) {
          qalloc->p.user_data = parent_key;
        }
        inserted = sc_hash_insert_unique (hash[qalloc->level], qalloc, NULL);
        P4EST_ASSERT (inserted);
        olist = &outlist[qalloc->level];
        sc_array_resize (olist, olist->elem_count + 1);
        qpointer = sc_array_index (olist, olist->elem_count - 1);
        *qpointer = qalloc;
        /* we need a new quadrant now, the old one is stored away */
        qalloc = sc_mempool_alloc (qpool);
        qalloc->p.user_data = key;
      }
    }
  }
  sc_mempool_free (qpool, qalloc);

  /* merge outlist into input list and free temporary storage */
  P4EST_LDEBUGF ("Hash statistics for tree %d\n", which_tree);
  curcount = inlist->elem_count;
  for (l = 0; l <= inmaxl; ++l) {
    /* print statistics and free hash tables */
#ifdef P4EST_DEBUG
    sc_hash_print_statistics (SC_LP_DEBUG, hash[l]);
#endif /* P4EST_DEBUG */
    sc_hash_unlink_destroy (hash[l]);   /* performance optimization */

    /* merge valid quadrants from outlist into inlist */
    ocount = outlist[l].elem_count;
    q = NULL;
    for (i = 0; i < ocount; ++i) {
      /* go through output list */
      qpointer = sc_array_index (&outlist[l], i);
      qalloc = *qpointer;
      P4EST_ASSERT (qalloc->level == l);
      P4EST_ASSERT (qalloc->p.user_data == key ||
                    qalloc->p.user_data == parent_key);
      if (p4est_quadrant_is_inside (qalloc)) {
        /* copy temporary quadrant into final tree */
        sc_array_resize (inlist, curcount + 1);
        q = sc_array_index (inlist, curcount);
        *q = *qalloc;
        ++curcount;
        ++tree->quadrants_per_level[l];

        /* complete quadrant initialization */
        p4est_quadrant_init_data (p4est, which_tree, q, init_fn);
      }
      else {
        P4EST_ASSERT (p4est_quadrant_is_extended (qalloc));
      }
      sc_mempool_free (qpool, qalloc);
    }
    if (q != NULL && l > tree->maxlevel) {
      tree->maxlevel = (int8_t) l;
    }
    sc_array_reset (&outlist[l]);
  }
#ifdef P4EST_DEBUG
  sc_mempool_reset (list_alloc);
#endif
  sc_mempool_destroy (list_alloc);

  /* print more statistics */
  P4EST_VERBOSEF ("Tree %d Outside root %d tree %d\n",
                  which_tree, count_outside_root, count_outside_tree);
  P4EST_INFOF ("Tree %d Already in inlist %d outlist %d insertions %d\n",
               which_tree, count_already_inlist, count_already_outlist,
               curcount - incount);

  /* sort and linearize tree */
  sc_array_sort (inlist, p4est_quadrant_compare);
  p4est_linearize_subtree (p4est, tree);

  /* run sanity checks */
  P4EST_ASSERT (quadrant_pool_size == qpool->elem_count);
  if (p4est->user_data_pool != NULL) {
    P4EST_ASSERT (data_pool_size + inlist->elem_count ==
                  p4est->user_data_pool->elem_count + incount);
  }
  P4EST_ASSERT (p4est_tree_is_linear (tree));
}

void
p4est_complete_subtree (p4est_t * p4est, p4est_tree_t * tree,
                        p4est_topidx_t which_tree, p4est_init_t init_fn)
{
  p4est_complete_or_balance (p4est, tree, 0, which_tree, init_fn);
}

void
p4est_balance_subtree (p4est_t * p4est, p4est_tree_t * tree,
                       p4est_topidx_t which_tree, p4est_init_t init_fn)
{
  p4est_complete_or_balance (p4est, tree, 2, which_tree, init_fn);
}

void
p4est_linearize_subtree (p4est_t * p4est, p4est_tree_t * tree)
{
  int                 data_pool_size = -1;
  int                 incount, removed;
  int                 current, rest, num_quadrants;
  int                 i, maxlevel;
  p4est_quadrant_t   *q1, *q2;
  sc_array_t         *tquadrants = &tree->quadrants;

  P4EST_ASSERT (p4est_tree_is_almost_sorted (tree, 0));

  incount = tquadrants->elem_count;
  if (incount <= 1) {
    return;
  }
  if (p4est->user_data_pool != NULL) {
    data_pool_size = p4est->user_data_pool->elem_count;
  }
  removed = 0;

  /* run through the array and remove ancestors */
  current = 0;
  rest = current + 1;
  q1 = sc_array_index (tquadrants, current);
  while (rest < incount) {
    q2 = sc_array_index (tquadrants, rest);
    if (p4est_quadrant_is_equal (q1, q2) ||
        p4est_quadrant_is_ancestor (q1, q2)) {
      --tree->quadrants_per_level[q1->level];
      p4est_quadrant_free_data (p4est, q1);
      *q1 = *q2;
      ++removed;
      ++rest;
    }
    else {
      ++current;
      if (current < rest) {
        q1 = sc_array_index (tquadrants, current);
        *q1 = *q2;
      }
      else {
        q1 = q2;
      }
      ++rest;
    }
  }

  /* resize array */
  sc_array_resize (tquadrants, current + 1);

  /* update level counters */
  maxlevel = 0;
  num_quadrants = 0;
  for (i = 0; i <= P4EST_MAXLEVEL; ++i) {
    P4EST_ASSERT (tree->quadrants_per_level[i] >= 0);
    num_quadrants += tree->quadrants_per_level[i];
    if (tree->quadrants_per_level[i] > 0) {
      maxlevel = i;
    }
  }
  tree->maxlevel = (int8_t) maxlevel;

  /* sanity checks */
  P4EST_ASSERT (num_quadrants == tquadrants->elem_count);
  P4EST_ASSERT (tquadrants->elem_count == incount - removed);
  if (p4est->user_data_pool != NULL) {
    P4EST_ASSERT (data_pool_size - removed ==
                  p4est->user_data_pool->elem_count);
  }
  P4EST_ASSERT (p4est_tree_is_sorted (tree));
  P4EST_ASSERT (p4est_tree_is_linear (tree));
}

p4est_gloidx_t
p4est_partition_given (p4est_t * p4est,
                       const p4est_locidx_t * new_num_quadrants_in_proc)
{
  const int           num_procs = p4est->mpisize;
  const int           rank = p4est->mpirank;
  const p4est_gloidx_t *global_last_quad_index =
    p4est->global_last_quad_index;
  const p4est_topidx_t first_local_tree = p4est->first_local_tree;
  const p4est_topidx_t last_local_tree = p4est->last_local_tree;
  const size_t        data_size = p4est->data_size;
  const size_t        quad_plus_data_size = sizeof (p4est_quadrant_t)
    + data_size;
  sc_array_t         *trees = p4est->trees;

  const p4est_topidx_t num_send_trees =
    p4est->global_first_position[rank + 1].which_tree
    - p4est->global_first_position[rank].which_tree + 1;

  int32_t             i, j, sk, which_tree, num_copy;
  int32_t             first_tree, last_tree;
  int32_t             from_proc, to_proc;
  int32_t             num_quadrants;
  int32_t             num_proc_recv_from, num_proc_send_to, num_recv_trees;
  int32_t             new_first_local_tree, new_last_local_tree;
  int32_t             new_local_num_quadrants;
  int32_t             first_from_tree, last_from_tree, from_tree;
  int32_t            *num_recv_from, *num_send_to;
  int32_t            *new_local_tree_elem_count;
  int32_t            *new_local_tree_elem_count_before;
  int64_t            *begin_send_to;
  int64_t             tree_from_begin, tree_from_end;
  int64_t             from_begin, from_end;
  int64_t             to_begin, to_end;
  int64_t             my_base, my_begin, my_end;
  int64_t            *new_global_last_quad_index;
  int64_t            *local_tree_last_quad_index;
  int64_t             diff64, total_quadrants_shipped;
  char              **recv_buf, **send_buf;
  sc_array_t         *quadrants;
  int32_t            *num_per_tree_local;
  int32_t            *num_per_tree_send_buf;
  int32_t            *num_per_tree_recv_buf;
  p4est_quadrant_t   *quad_send_buf;
  p4est_quadrant_t   *quad_recv_buf;
  p4est_quadrant_t   *quad;
  p4est_tree_t       *tree;
  char               *user_data_send_buf;
  char               *user_data_recv_buf;
  size_t              recv_size, send_size;
#ifdef P4EST_MPI
  int                 mpiret;
  MPI_Comm            comm = p4est->mpicomm;
  MPI_Request        *recv_request, *send_request;
  MPI_Status         *recv_status, *send_status;
#endif
#ifdef P4EST_DEBUG
  unsigned            crc;
  int64_t             total_requested_quadrants = 0;
#endif

  P4EST_GLOBAL_INFOF
    ("Into p4est_partition_given with %lld total quadrants\n",
     (long long) p4est->global_num_quadrants);

#ifdef P4EST_DEBUG
  /* Save a checksum of the original forest */
  crc = p4est_checksum (p4est);

  /* Check for a valid requested partition */
  for (i = 0; i < num_procs; ++i) {
    total_requested_quadrants += new_num_quadrants_in_proc[i];
    P4EST_ASSERT (new_num_quadrants_in_proc[i] >= 0);
  }
  P4EST_ASSERT (total_requested_quadrants == p4est->global_num_quadrants);
#endif

  /* Print some diagnostics */
  if (rank == 0) {
    for (i = 0; i < num_procs; ++i) {
      P4EST_GLOBAL_VERBOSEF ("partition global_last_quad_index[%d] = %lld\n",
                             i, (long long) global_last_quad_index[i]);
    }
  }

  /* Calculate the global_last_quad_index for the repartitioned forest */
  new_global_last_quad_index = P4EST_ALLOC (int64_t, num_procs);
  new_global_last_quad_index[0] = new_num_quadrants_in_proc[0] - 1;
  for (i = 1; i < num_procs; ++i) {
    new_global_last_quad_index[i] = new_num_quadrants_in_proc[i] +
      new_global_last_quad_index[i - 1];
  }
  P4EST_ASSERT (global_last_quad_index[num_procs - 1] ==
                new_global_last_quad_index[num_procs - 1]);

  /* Calculate the global number of shipped quadrants */
  total_quadrants_shipped = 0;
  for (i = 1; i < num_procs; ++i) {
    diff64 =
      global_last_quad_index[i - 1] - new_global_last_quad_index[i - 1];
    if (diff64 >= 0) {
      total_quadrants_shipped +=
        SC_MIN (diff64, new_num_quadrants_in_proc[i]);
    }
    else {
      total_quadrants_shipped +=
        SC_MIN (-diff64, new_num_quadrants_in_proc[i - 1]);
    }
  }
  P4EST_ASSERT (0 <= total_quadrants_shipped &&
                total_quadrants_shipped <= p4est->global_num_quadrants);

  /* Print some diagnostics */
  if (rank == 0) {
    for (i = 0; i < num_procs; ++i) {
      P4EST_GLOBAL_VERBOSEF
        ("partition new_global_last_quad_index[%d] = %lld\n",
         i, (long long) new_global_last_quad_index[i]);
    }
  }

  /* Calculate the local index of the end of each tree */
  local_tree_last_quad_index = P4EST_ALLOC_ZERO (int64_t, trees->elem_count);
  if (first_local_tree >= 0) {
    tree = sc_array_index (p4est->trees, first_local_tree);
    local_tree_last_quad_index[first_local_tree]
      = tree->quadrants.elem_count - 1;
  }
  else {
    P4EST_ASSERT (first_local_tree == -1 && last_local_tree == -2);
  }
  for (which_tree = first_local_tree + 1; which_tree <= last_local_tree;
       ++which_tree) {
    tree = sc_array_index (p4est->trees, which_tree);
    local_tree_last_quad_index[which_tree] = tree->quadrants.elem_count
      + local_tree_last_quad_index[which_tree - 1];
  }

#ifdef P4EST_DEBUG
  for (which_tree = first_local_tree; which_tree <= last_local_tree;
       ++which_tree) {
    tree = sc_array_index (p4est->trees, which_tree);
    P4EST_LDEBUGF
      ("partition tree %d local_tree_last_quad_index[%d] = %lld\n",
       which_tree, which_tree,
       (long long) local_tree_last_quad_index[which_tree]);
  }
#endif

  /* Calculate where the quadrants are coming from */
  num_recv_from = P4EST_ALLOC (int32_t, num_procs);

  my_begin = (rank == 0) ? 0 : (new_global_last_quad_index[rank - 1] + 1);
  my_end = new_global_last_quad_index[rank];

  num_proc_recv_from = 0;
  for (from_proc = 0; from_proc < num_procs; ++from_proc) {
    from_begin = (from_proc == 0) ?
      0 : (global_last_quad_index[from_proc - 1] + 1);
    from_end = global_last_quad_index[from_proc];

    if (from_begin <= my_end && from_end >= my_begin) {
      /* from_proc sends to me */
      num_recv_from[from_proc] = SC_MIN (my_end, from_end)
        - SC_MAX (my_begin, from_begin) + 1;
      if (from_proc != rank)
        ++num_proc_recv_from;
    }
    else {
      /* from_proc does not send to me */
      num_recv_from[from_proc] = 0;
    }
  }

#ifdef P4EST_DEBUG
  for (i = 0; i < num_procs; ++i) {
    if (num_recv_from[i] != 0) {
      P4EST_LDEBUGF ("partition num_recv_from[%d] = %d\n", i,
                     num_recv_from[i]);
    }
  }
#endif

  /* Post receives for the quadrants and their data */
  recv_buf = P4EST_ALLOC (char *, num_procs);
#ifdef P4EST_MPI
  recv_request = P4EST_ALLOC (MPI_Request, num_proc_recv_from);
  recv_status = P4EST_ALLOC (MPI_Status, num_proc_recv_from);
#endif

  /* Allocate space for receiving quadrants and user data */
  for (from_proc = 0, sk = 0; from_proc < num_procs; ++from_proc) {
    if (from_proc != rank && num_recv_from[from_proc]) {
      num_recv_trees = p4est->global_first_position[from_proc + 1].which_tree
        - p4est->global_first_position[from_proc].which_tree + 1;
      recv_size = num_recv_trees * sizeof (int32_t)
        + quad_plus_data_size * num_recv_from[from_proc];

      recv_buf[from_proc] = P4EST_ALLOC (char, recv_size);

      /* Post receives for the quadrants and their data */
#ifdef P4EST_MPI
      P4EST_LDEBUGF ("partition recv %d quadrants from %d\n",
                     num_recv_from[from_proc], from_proc);
      mpiret = MPI_Irecv (recv_buf[from_proc], recv_size, MPI_BYTE,
                          from_proc, P4EST_COMM_PARTITION_GIVEN,
                          comm, recv_request + sk);
      SC_CHECK_MPI (mpiret);
#endif
      ++sk;
    }
    else {
      recv_buf[from_proc] = NULL;
    }
  }

  /* For each processor calculate the number of quadrants sent */
  num_send_to = P4EST_ALLOC (int32_t, num_procs);
  begin_send_to = P4EST_ALLOC (int64_t, num_procs);

  my_begin = (rank == 0) ? 0 : (global_last_quad_index[rank - 1] + 1);
  my_end = global_last_quad_index[rank];

  num_proc_send_to = 0;
  for (to_proc = 0; to_proc < num_procs; ++to_proc) {
    to_begin = (to_proc == 0)
      ? 0 : (new_global_last_quad_index[to_proc - 1] + 1);
    to_end = new_global_last_quad_index[to_proc];

    if (to_begin <= my_end && to_end >= my_begin) {
      /* I send to to_proc */
      num_send_to[to_proc] = SC_MIN (my_end, to_end)
        - SC_MAX (my_begin, to_begin) + 1;
      begin_send_to[to_proc] = SC_MAX (my_begin, to_begin);
      if (to_proc != rank)
        ++num_proc_send_to;
    }
    else {
      /* I don't send to to_proc */
      num_send_to[to_proc] = 0;
      begin_send_to[to_proc] = -1;
    }

  }

#ifdef P4EST_DEBUG
  for (i = 0; i < num_procs; ++i) {
    if (num_send_to[i] != 0) {
      P4EST_LDEBUGF ("partition num_send_to[%d] = %d\n", i, num_send_to[i]);
    }
  }
  for (i = 0; i < num_procs; ++i) {
    if (begin_send_to[i] >= 0) {
      P4EST_LDEBUGF ("partition begin_send_to[%d] = %lld\n",
                     i, (long long) begin_send_to[i]);
    }
  }
#endif

  /* Communicate the quadrants and their data */
  send_buf = P4EST_ALLOC (char *, num_procs);
#ifdef P4EST_MPI
  send_request = P4EST_ALLOC (MPI_Request, num_proc_send_to);
  send_status = P4EST_ALLOC (MPI_Status, num_proc_send_to);
#endif

  /* Set the num_per_tree_local */
  num_per_tree_local = P4EST_ALLOC_ZERO (int32_t, num_send_trees);
  to_proc = rank;
  my_base = (rank == 0) ? 0 : (global_last_quad_index[rank - 1] + 1);
  my_begin = begin_send_to[to_proc] - my_base;
  my_end = begin_send_to[to_proc] + num_send_to[to_proc] - 1 - my_base;
  for (which_tree = first_local_tree; which_tree <= last_local_tree;
       ++which_tree) {
    tree = sc_array_index (p4est->trees, which_tree);

    from_begin = (which_tree == first_local_tree) ? 0 :
      (local_tree_last_quad_index[which_tree - 1] + 1);
    from_end = local_tree_last_quad_index[which_tree];

    if (from_begin <= my_end && from_end >= my_begin) {
      /* Need to copy from tree which_tree */
      tree_from_begin = SC_MAX (my_begin, from_begin) - from_begin;
      tree_from_end = SC_MIN (my_end, from_end) - from_begin;
      num_copy = tree_from_end - tree_from_begin + 1;

      num_per_tree_local[which_tree - first_local_tree] = num_copy;
    }
  }

  /* Allocate space for receiving quadrants and user data */
  for (to_proc = 0, sk = 0; to_proc < num_procs; ++to_proc) {
    if (to_proc != rank && num_send_to[to_proc]) {
      send_size = num_send_trees * sizeof (int32_t)
        + quad_plus_data_size * num_send_to[to_proc];

      send_buf[to_proc] = P4EST_ALLOC (char, send_size);

      num_per_tree_send_buf = (int32_t *) send_buf[to_proc];
      memset (num_per_tree_send_buf, 0, num_send_trees * sizeof (int32_t));
      quad_send_buf = (p4est_quadrant_t *) (send_buf[to_proc]
                                            +
                                            num_send_trees *
                                            sizeof (int32_t));
      user_data_send_buf =
        send_buf[to_proc] + num_send_trees * sizeof (int32_t)
        + num_send_to[to_proc] * sizeof (p4est_quadrant_t);

      /* Pack in the data to be sent */

      my_base = (rank == 0) ? 0 : (global_last_quad_index[rank - 1] + 1);
      my_begin = begin_send_to[to_proc] - my_base;
      my_end = begin_send_to[to_proc] + num_send_to[to_proc] - 1 - my_base;

      for (which_tree = first_local_tree; which_tree <= last_local_tree;
           ++which_tree) {
        tree = sc_array_index (p4est->trees, which_tree);

        from_begin = (which_tree == first_local_tree) ? 0 :
          (local_tree_last_quad_index[which_tree - 1] + 1);
        from_end = local_tree_last_quad_index[which_tree];

        if (from_begin <= my_end && from_end >= my_begin) {
          /* Need to copy from tree which_tree */
          tree_from_begin = SC_MAX (my_begin, from_begin) - from_begin;
          tree_from_end = SC_MIN (my_end, from_end) - from_begin;
          num_copy = tree_from_end - tree_from_begin + 1;

          num_per_tree_send_buf[which_tree - first_local_tree] = num_copy;

          /* copy quads to send buf */
          memcpy (quad_send_buf, tree->quadrants.array +
                  tree_from_begin * sizeof (p4est_quadrant_t),
                  num_copy * sizeof (p4est_quadrant_t));

          /* set tree in send buf and copy user data */
          P4EST_LDEBUGF ("partition send %d [%lld,%lld] quadrants"
                         " from tree %d to proc %d\n",
                         num_copy, (long long) tree_from_begin,
                         (long long) tree_from_end, which_tree, to_proc);
          for (i = 0; i < num_copy; ++i) {
            memcpy (user_data_send_buf + i * data_size,
                    quad_send_buf[i].p.user_data, data_size);
            quad_send_buf[i].p.user_data = NULL;

          }

          /* move the pointer to the begining of the quads that need copied */
          my_begin += num_copy;
          quad_send_buf += num_copy;
          user_data_send_buf += num_copy * data_size;
        }
      }

      /* Post receives for the quadrants and their data */
#ifdef P4EST_MPI
      P4EST_LDEBUGF ("partition send %d quadrants to %d\n",
                     num_send_to[to_proc], to_proc);
      mpiret = MPI_Isend (send_buf[to_proc], send_size, MPI_BYTE,
                          to_proc, P4EST_COMM_PARTITION_GIVEN,
                          comm, send_request + sk);
      SC_CHECK_MPI (mpiret);
      ++sk;
#endif
    }
    else {
      send_buf[to_proc] = NULL;
    }
  }

  /* Fill in forest */
#ifdef P4EST_MPI
  mpiret = MPI_Waitall (num_proc_recv_from, recv_request, recv_status);
  SC_CHECK_MPI (mpiret);
#endif

  /* Loop Through and fill in */

  /* Calculate the local index of the end of each tree in the repartition */
  new_local_tree_elem_count = P4EST_ALLOC_ZERO (int32_t, trees->elem_count);
  new_local_tree_elem_count_before = P4EST_ALLOC_ZERO (int32_t,
                                                       trees->elem_count);
  new_first_local_tree = trees->elem_count;
  new_last_local_tree = 0;

  for (from_proc = 0; from_proc < num_procs; ++from_proc) {
    if (num_recv_from[from_proc] > 0) {
      first_from_tree = p4est->global_first_position[from_proc].which_tree;
      last_from_tree = p4est->global_first_position[from_proc + 1].which_tree;
      num_recv_trees = last_from_tree - first_from_tree + 1;

      P4EST_LDEBUGF ("partition from %d with trees [%d,%d] get %d trees\n",
                     from_proc, first_from_tree, last_from_tree,
                     num_recv_trees);
      num_per_tree_recv_buf =
        (from_proc ==
         rank) ? num_per_tree_local : (int32_t *) recv_buf[from_proc];

      for (i = 0; i < num_recv_trees; ++i) {

        if (num_per_tree_recv_buf[i] > 0) {
          from_tree = first_from_tree + i;

          P4EST_ASSERT (from_tree >= 0 && from_tree < trees->elem_count);
          P4EST_LDEBUGF ("partition recv %d [%d,%d] quadrants"
                         " from tree %d from proc %d\n",
                         num_per_tree_recv_buf[i],
                         new_local_tree_elem_count[from_tree],
                         new_local_tree_elem_count[from_tree]
                         + num_per_tree_recv_buf[i], from_tree, from_proc);
          new_first_local_tree = SC_MIN (new_first_local_tree, from_tree);
          new_last_local_tree = SC_MAX (new_last_local_tree, from_tree);
          new_local_tree_elem_count[from_tree] += num_per_tree_recv_buf[i];
          new_local_tree_elem_count_before[from_tree]
            += (from_proc < rank) ? num_per_tree_recv_buf[i] : 0;
        }
      }
    }
  }
  if (new_first_local_tree > new_last_local_tree) {
    new_first_local_tree = -1;
    new_last_local_tree = -2;
  }
  P4EST_INFOF ("partition new forest [%d,%d]\n",
               new_first_local_tree, new_last_local_tree);

  /* Copy the local quadrants */
  if (first_local_tree >= 0 && new_first_local_tree >= 0) {
    P4EST_ASSERT (last_local_tree >= 0 && new_last_local_tree >= 0);
    first_tree = SC_MIN (first_local_tree, new_first_local_tree);
  }
  else {
    P4EST_ASSERT (last_local_tree == -2 || new_last_local_tree == -2);
    first_tree = SC_MAX (first_local_tree, new_first_local_tree);
  }
  last_tree = SC_MAX (last_local_tree, new_last_local_tree);
  my_base = (rank == 0) ? 0 : (global_last_quad_index[rank - 1] + 1);
  my_begin = begin_send_to[rank] - my_base;
  my_end = begin_send_to[rank] + num_send_to[rank] - 1 - my_base;

  for (which_tree = first_tree; which_tree <= last_tree; ++which_tree) {
    tree = sc_array_index (p4est->trees, which_tree);
    quadrants = &tree->quadrants;

    if (new_local_tree_elem_count[which_tree] > 0) {
      if (which_tree >= first_local_tree && which_tree <= last_local_tree) {

        num_quadrants = new_local_tree_elem_count[which_tree];

        from_begin = (which_tree == first_local_tree) ? 0 :
          (local_tree_last_quad_index[which_tree - 1] + 1);
        from_end = local_tree_last_quad_index[which_tree];

        if (from_begin <= my_end && from_end >= my_begin) {
          /* Need to keep part of tree which_tree */
          tree_from_begin = SC_MAX (my_begin, from_begin) - from_begin;
          tree_from_end = SC_MIN (my_end, from_end) - from_begin;
          num_copy = tree_from_end - tree_from_begin + 1;
        }
        else {
          tree_from_begin = 0;
          tree_from_end = -1;
          num_copy = 0;
        }

        /* Free all userdata that no longer belongs to this process */
        for (i = 0; i < quadrants->elem_count; ++i) {
          if (i < tree_from_begin || i > tree_from_end) {
            quad = sc_array_index (quadrants, i);
            p4est_quadrant_free_data (p4est, quad);
          }
        }

        if (num_quadrants > quadrants->elem_count) {
          sc_array_resize (quadrants, num_quadrants);
        }

        P4EST_LDEBUGF ("copying %d local quads to tree %d\n",
                       num_copy, which_tree);
        P4EST_LDEBUGF
          ("   with %d(%lu) quads from [%lld, %lld] to [%d, %d]\n",
           num_quadrants, (unsigned long) quadrants->elem_count,
           (long long) tree_from_begin, (long long) tree_from_end,
           new_local_tree_elem_count_before[which_tree],
           new_local_tree_elem_count_before[which_tree] + num_copy - 1);
        memmove (quadrants->array +
                 new_local_tree_elem_count_before[which_tree] *
                 sizeof (p4est_quadrant_t),
                 quadrants->array +
                 tree_from_begin * sizeof (p4est_quadrant_t),
                 num_copy * sizeof (p4est_quadrant_t));

        if (num_quadrants < quadrants->elem_count) {
          sc_array_resize (quadrants, num_quadrants);
        }
      }
    }
    else {
      /*
       * Check to see if we need to drop a tree because we no longer have
       * any quadrants in it.
       */
      if (which_tree >= first_local_tree && which_tree <= last_local_tree) {
        /* Free all userdata that no longer belongs to this process */
        for (i = 0; i < quadrants->elem_count; ++i) {
          quad = sc_array_index (quadrants, i);
          p4est_quadrant_free_data (p4est, quad);
        }

        /* The whole tree is dropped */
        sc_array_reset (quadrants);
        for (i = 0; i <= P4EST_MAXLEVEL; ++i) {
          tree->quadrants_per_level[i] = 0;
        }
        tree->maxlevel = 0;
      }
    }
  }

  /* Copy in received quadrants */

  memset (new_local_tree_elem_count_before, 0,
          trees->elem_count * sizeof (int32_t));
  for (from_proc = 0; from_proc < num_procs; ++from_proc) {
    if (num_recv_from[from_proc] > 0) {
      first_from_tree = p4est->global_first_position[from_proc].which_tree;
      last_from_tree = p4est->global_first_position[from_proc + 1].which_tree;
      num_recv_trees = last_from_tree - first_from_tree + 1;

      P4EST_LDEBUGF
        ("partition copy from %d with trees [%d,%d] get %d trees\n",
         from_proc, first_from_tree, last_from_tree, num_recv_trees);
      num_per_tree_recv_buf =
        (from_proc == rank) ? num_per_tree_local :
        (int32_t *) recv_buf[from_proc];

      quad_recv_buf = (p4est_quadrant_t *) (recv_buf[from_proc]
                                            + num_recv_trees *
                                            sizeof (int32_t));
      user_data_recv_buf =
        recv_buf[from_proc] + num_recv_trees * sizeof (int32_t)
        + num_recv_from[from_proc] * sizeof (p4est_quadrant_t);

      for (i = 0; i < num_recv_trees; ++i) {
        from_tree = first_from_tree + i;
        num_copy = num_per_tree_recv_buf[i];

        /* We might have sent trees that are not actual trees.  In
         * this case the num_copy should be zero
         */
        P4EST_ASSERT (num_copy == 0
                      || (num_copy > 0 && from_tree >= 0
                          && from_tree < trees->elem_count));

        if (num_copy > 0 && rank != from_proc) {
          tree = sc_array_index (p4est->trees, from_tree);
          quadrants = &tree->quadrants;
          num_quadrants = new_local_tree_elem_count[from_tree];
          sc_array_resize (quadrants, num_quadrants);

          /* copy quadrants */
          P4EST_LDEBUGF ("copying %d remote quads to tree %d"
                         " with %d quads from proc %d\n",
                         num_copy, from_tree, num_quadrants, from_proc);
          memcpy (quadrants->array +
                  new_local_tree_elem_count_before[from_tree]
                  * sizeof (p4est_quadrant_t), quad_recv_buf,
                  num_copy * sizeof (p4est_quadrant_t));

          /* copy user data */
          for (j = 0; j < num_copy; ++j) {
            quad = sc_array_index (quadrants,
                                   j +
                                   new_local_tree_elem_count_before
                                   [from_tree]);

            if (data_size > 0) {
              quad->p.user_data = sc_mempool_alloc (p4est->user_data_pool);
              memcpy (quad->p.user_data, user_data_recv_buf + j * data_size,
                      data_size);
            }
            else {
              quad->p.user_data = NULL;
            }
          }
        }

        if (num_copy > 0) {
          P4EST_ASSERT (from_tree >= 0 && from_tree < trees->elem_count);
          new_local_tree_elem_count_before[from_tree] += num_copy;
        }

        /* increment the recv quadrant pointers */
        quad_recv_buf += num_copy;
        user_data_recv_buf += num_copy * data_size;
      }
      if (recv_buf[from_proc] != NULL) {
        P4EST_FREE (recv_buf[from_proc]);
        recv_buf[from_proc] = NULL;
      }
    }
  }

  /* Set the global index and count of quadrants instead
   * of calling p4est_comm_count_quadrants
   */
  P4EST_FREE (p4est->global_last_quad_index);
  p4est->global_last_quad_index = new_global_last_quad_index;
  P4EST_ASSERT (p4est->global_num_quadrants ==
                new_global_last_quad_index[num_procs - 1] + 1);

  p4est->first_local_tree = new_first_local_tree;
  p4est->last_local_tree = new_last_local_tree;

  new_local_num_quadrants = 0;
  for (which_tree = new_first_local_tree; which_tree <= new_last_local_tree;
       ++which_tree) {
    tree = sc_array_index (p4est->trees, which_tree);
    quadrants = &tree->quadrants;

    new_local_num_quadrants += quadrants->elem_count;

    for (i = 0; i <= P4EST_MAXLEVEL; ++i) {
      tree->quadrants_per_level[i] = 0;
    }
    tree->maxlevel = 0;
    for (i = 0; i < quadrants->elem_count; ++i) {
      quad = sc_array_index (quadrants, i);
      ++tree->quadrants_per_level[quad->level];
      tree->maxlevel = (int8_t) SC_MAX (quad->level, tree->maxlevel);
    }
  }
  p4est->local_num_quadrants = new_local_num_quadrants;

  /* Clean up */

#ifdef P4EST_MPI
  mpiret = MPI_Waitall (num_proc_send_to, send_request, send_status);
  SC_CHECK_MPI (mpiret);

#ifdef P4EST_DEBUG
  for (i = 0; i < num_proc_recv_from; ++i) {
    P4EST_ASSERT (recv_request[i] == MPI_REQUEST_NULL);
  }
  for (i = 0; i < num_proc_send_to; ++i) {
    P4EST_ASSERT (send_request[i] == MPI_REQUEST_NULL);
  }
#endif
  P4EST_FREE (recv_request);
  P4EST_FREE (recv_status);
  P4EST_FREE (send_request);
  P4EST_FREE (send_status);
#endif

  for (i = 0; i < num_procs; ++i) {
    if (recv_buf[i] != NULL)
      P4EST_FREE (recv_buf[i]);
    if (send_buf[i] != NULL)
      P4EST_FREE (send_buf[i]);
  }

  P4EST_FREE (num_per_tree_local);
  P4EST_FREE (local_tree_last_quad_index);
  P4EST_FREE (new_local_tree_elem_count);
  P4EST_FREE (new_local_tree_elem_count_before);
  P4EST_FREE (recv_buf);
  P4EST_FREE (send_buf);
  P4EST_FREE (num_recv_from);
  P4EST_FREE (num_send_to);
  P4EST_FREE (begin_send_to);

  p4est_comm_global_partition (p4est);

  /* Assert that we have a valid partition */
  P4EST_ASSERT (crc == p4est_checksum (p4est));
  P4EST_GLOBAL_INFOF
    ("Done p4est_partition_given shipped %lld quadrants %.3g%%\n",
     (long long) total_quadrants_shipped,
     total_quadrants_shipped * 100. / p4est->global_num_quadrants);

  return total_quadrants_shipped;
}

/* EOF p4est_algorithms.c */