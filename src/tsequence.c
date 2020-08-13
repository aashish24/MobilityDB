/*****************************************************************************
 *
 * tsequence.c
 *	  Basic functions for temporal sequences.
 *
 * Portions Copyright (c) 2020, Esteban Zimanyi, Arthur Lesuisse, 
 * 		Universite Libre de Bruxelles
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *****************************************************************************/

#include "tsequence.h"

#include <assert.h>
#include <float.h>
#include <access/hash.h>
#include <libpq/pqformat.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/timestamp.h>

#include "timestampset.h"
#include "period.h"
#include "periodset.h"
#include "timeops.h"
#include "doublen.h"
#include "temporaltypes.h"
#include "oidcache.h"
#include "temporal_util.h"
#include "temporal_boxops.h"
#include "rangetypes_ext.h"

#include "tpoint.h"
#include "tpoint_boxops.h"
#include "tpoint_spatialfuncs.h"
#include "tpoint_distance.h"

/*****************************************************************************
 * Compute the intersection, if any, of a segment of a temporal sequence and
 * a value. The functions only return true when there is an intersection at
 * the middle of the segment, i.e., they return false if they intersect at a
 * bound. When they return true, they also return in the output parameter
 * the intersection timestampt t. The value taken by the segment and the
 * target value are equal up to the floating point precision.
 * There is no need to add functions for DoubleN, which are used for computing
 * avg and centroid aggregates, since these computations are based on sum and
 * thus they do not need to add intermediate points.
 *****************************************************************************/

/**
 * Returns true if the segment of the temporal number intersects
 * the base value at the timestamp
 *
 * @param[in] inst1,inst2 Temporal instants defining the segment
 * @param[in] value Base value 
 * @param[in] valuetypid Oid of the base type
 * @param[out] t Timestamp 
 */
static bool
tnumberseq_intersection_value(const TInstant *inst1, 
	const TInstant *inst2, Datum value, Oid valuetypid, TimestampTz *t)
{
	assert(inst1->valuetypid == FLOAT8OID);
	double dvalue1 = DatumGetFloat8(tinstant_value(inst1));
	double dvalue2 = DatumGetFloat8(tinstant_value(inst2));
	double dvalue = datum_double(value, valuetypid);
	double min = Min(dvalue1, dvalue2);
	double max = Max(dvalue1, dvalue2);
	/* if value is to the left or to the right of the range */
	if (dvalue < min || dvalue > max)
		return false;

	double range = (max - min);
	double partial = (dvalue - min);
	double fraction = dvalue1 < dvalue2 ? partial / range : 1 - partial / range;
	if (fabs(fraction) < EPSILON || fabs(fraction - 1.0) < EPSILON)
		return false;

	if (t != NULL)
	{
		double duration = (inst2->t - inst1->t);
		*t = inst1->t + (long) (duration * fraction);
	}
	return true;
}

/**
 * Returns true if the segment of the temporal point value intersects
 * the base value at the timestamp
 *
 * @param[in] inst1,inst2 Temporal instants defining the segment
 * @param[in] value Base value 
 * @param[out] t Timestamp 
 */
static bool
tpointseq_intersection_value(const TInstant *inst1, const TInstant *inst2,
	Datum value, TimestampTz *t)
{
	GSERIALIZED *gs = (GSERIALIZED *)PG_DETOAST_DATUM(value);
	if (gserialized_is_empty(gs))
	{
		POSTGIS_FREE_IF_COPY_P(gs, DatumGetPointer(value));
		return false;
	}

	/* We are sure that the trajectory is a line */
	Datum start = tinstant_value(inst1);
	Datum end = tinstant_value(inst2);
	double dist;
	ensure_point_base_type(inst1->valuetypid);
	double fraction = inst1->valuetypid == type_oid(T_GEOMETRY) ?
		geomseg_locate_point(start, end, value, &dist) :
		geogseg_locate_point(start, end, value, &dist);
	if (dist >= EPSILON ||
		(fabs(fraction) < EPSILON || fabs(fraction - 1.0) < EPSILON))
		return false;

	if (t != NULL)
	{
		double duration = (inst2->t - inst1->t);
		*t = inst1->t + (long) (duration * fraction);
	}
	return true;
}

/**
 * Returns true if the segment of the temporal value intersects
 * the base value at the timestamp
 *
 * @param[in] inst1,inst2 Temporal instants defining the segment
 * @param[in] value Base value 
 * @param[in] valuetypid Base type 
 * @param[out] inter Base value taken by the segment at the timestamp.
 * This value is equal to the input base value up to the floating 
 * point precision.
 * @param[out] t Timestamp 
 */
bool
tlinearseq_intersection_value(const TInstant *inst1, const TInstant *inst2,
	Datum value, Oid valuetypid, Datum *inter, TimestampTz *t)
{
	Datum value1 = tinstant_value(inst1);
	Datum value2 = tinstant_value(inst2);
	if (datum_eq(value, value1, inst1->valuetypid) ||
		datum_eq(value, value2, inst1->valuetypid))
		return false;

	ensure_linear_interpolation(inst1->valuetypid);
	bool result = false; /* make compiler quiet */
	if (inst1->valuetypid == FLOAT8OID)
		result = tnumberseq_intersection_value(inst1, inst2, value,
			valuetypid, t);
	else if (point_base_type(inst1->valuetypid))
		result = tpointseq_intersection_value(inst1, inst2, value, t);

	if (result && inter != NULL)
		/* We are sure it is linear interpolation */
		*inter = tsequence_value_at_timestamp1(inst1, inst2, true, *t);
	return result;
}

/*****************************************************************************
 * Compute the intersection, if any, of two segments of temporal sequences.
 * These functions suppose that the instants are synchronized, i.e.,
 * start1->t = start2->t and end1->t = end2->t.
 * The functions return true if there is an intersection at the middle of
 * the segments, i.e., they return false if they intersect at a bound. If
 * they return true, they also return in the output parameter t the
 * intersection timestamp. The two values taken by the segments at the
 * intersection timestamp t are equal up to the floating point precision.
 * For the temporal point case we cannot use the PostGIS functions
 * lw_dist2d_seg_seg and lw_dist3d_seg_seg since they do not take time into
 * consideration and would return, e.g., that the two segments
 * [Point(1 1)@t1, Point(3 3)@t2] and [Point(3 3)@t1, Point(1 1)@t2]
 * intersect at Point(1 1), instead of Point(2 2).
 * These functions are used to add intermediate points when lifting
 * operators, in particular for temporal comparisons such as
 * tfloat <comp> tfloat where <comp> is <, <=, ... since the comparison
 * changes its value before/at/after the intersection point.
 *****************************************************************************/

/**
 * Returns true if the two segments of the temporal numbers
 * intersect at the timestamp
 *
 * @param[in] start1,end1 Temporal instants defining the first segment
 * @param[in] start2,end2 Temporal instants defining the second segment
 * @param[out] t Timestamp
 * @pre The instants are synchronized, i.e., start1->t = start2->t and 
 * end1->t = end2->t
 */
static bool
tnumberseq_intersection(const TInstant *start1, const TInstant *end1,
	const TInstant *start2, const TInstant *end2, TimestampTz *t)
{
	double x1 = datum_double(tinstant_value(start1), start1->valuetypid);
	double x2 = datum_double(tinstant_value(end1), start1->valuetypid);
	double x3 = datum_double(tinstant_value(start2), start2->valuetypid);
	double x4 = datum_double(tinstant_value(end2), start2->valuetypid);
	/* Compute the instant t at which the linear functions of the two segments
	   are equal: at + b = ct + d that is t = (d - b) / (a - c).
	   To reduce problems related to floating point arithmetic, t1 and t2
	   are shifted, respectively, to 0 and 1 before the computation */
	long double denum = x2 - x1 - x4 + x3;
	if (denum == 0)
		/* Parallel segments */
		return false;

	long double fraction = ((long double) (x3 - x1)) / denum;
	if (fraction <= EPSILON || fraction >= (1.0 - EPSILON))
		/* Intersection occurs out of the period */
		return false;

	double duration = (end1->t - start1->t);
	*t = start1->t + (long) (duration * fraction);
	return true;
}

/**
 * Returns true if the two segments of the temporal geometric point
 * values intersect at the timestamp
 *
 * @param[in] start1,end1 Temporal instants defining the first segment
 * @param[in] start2,end2 Temporal instants defining the second segment
 * @param[out] t Timestamp 
 * @pre The instants are synchronized, i.e., start1->t = start2->t and 
 * end1->t = end2->t
 */
bool
tgeompointseq_intersection(const TInstant *start1, const TInstant *end1,
	const TInstant *start2, const TInstant *end2, TimestampTz *t)
{
	long double fraction, xfraction = 0, yfraction = 0, xdenum, ydenum;
	if (MOBDB_FLAGS_GET_Z(start1->flags)) /* 3D */
	{
		long double zfraction = 0, zdenum;
		const POINT3DZ *p1 = datum_get_point3dz_p(tinstant_value(start1));
		const POINT3DZ *p2 = datum_get_point3dz_p(tinstant_value(end1));
		const POINT3DZ *p3 = datum_get_point3dz_p(tinstant_value(start2));
		const POINT3DZ *p4 = datum_get_point3dz_p(tinstant_value(end2));
		xdenum = p2->x - p1->x - p4->x + p3->x;
		ydenum = p2->y - p1->y - p4->y + p3->y;
		zdenum = p2->z - p1->z - p4->z + p3->z;
		if (xdenum == 0 && ydenum == 0 && zdenum == 0)
			/* Parallel segments */
			return false;

		if (xdenum != 0)
		{
			xfraction = (p3->x - p1->x) / xdenum;
			/* If intersection occurs out of the period */
			if (xfraction <= EPSILON || xfraction >= (1.0 - EPSILON))
				return false;
		}
		if (ydenum != 0)
		{
			yfraction = (p3->y - p1->y) / ydenum;
			/* If intersection occurs out of the period */
			if (yfraction <= EPSILON || yfraction >= (1.0 - EPSILON))
				return false;
		}
		if (zdenum != 0)
		{
			/* If intersection occurs out of the period or intersect 
			 * at different timestamps */
			zfraction = (p3->z - p1->z) / zdenum;
			if (zfraction <= EPSILON || zfraction >= (1.0 - EPSILON))
				return false;
		}
		/* If intersect at different timestamps on each dimension */
		if ((xdenum != 0 && ydenum != 0 && zdenum != 0 &&
			fabsl(xfraction - yfraction) > EPSILON && 
			fabsl(xfraction - zfraction) > EPSILON) ||
			(xdenum == 0 && ydenum != 0 && zdenum != 0 &&
			fabsl(yfraction - zfraction) > EPSILON) ||
			(xdenum != 0 && ydenum == 0 && zdenum != 0 &&
			fabsl(xfraction - zfraction) > EPSILON) ||
			(xdenum != 0 && ydenum != 0 && zdenum == 0 &&
			fabsl(xfraction - yfraction) > EPSILON))
			return false;
		if (xdenum != 0)
			fraction = xfraction;
		else if (ydenum != 0)
			fraction = yfraction;
		else
			fraction = zfraction;
	}
	else /* 2D */
	{
		const POINT2D *p1 = datum_get_point2d_p(tinstant_value(start1));
		const POINT2D *p2 = datum_get_point2d_p(tinstant_value(end1));
		const POINT2D *p3 = datum_get_point2d_p(tinstant_value(start2));
		const POINT2D *p4 = datum_get_point2d_p(tinstant_value(end2));
		xdenum = p2->x - p1->x - p4->x + p3->x;
		ydenum = p2->y - p1->y - p4->y + p3->y;
		if (xdenum == 0 && ydenum == 0)
			/* Parallel segments */
			return false;

		if (xdenum != 0)
		{
			xfraction = (p3->x - p1->x) / xdenum;
			/* If intersection occurs out of the period */
			if (xfraction <= EPSILON || xfraction >= (1.0 - EPSILON))
				return false;
		}
		if (ydenum != 0)
		{
			yfraction = (p3->y - p1->y) / ydenum;
			/* If intersection occurs out of the period */
			if (yfraction <= EPSILON || yfraction >= (1.0 - EPSILON))
				return false;
		}
		/* If intersect at different timestamps on each dimension */
		if (xdenum != 0 && ydenum != 0 && fabsl(xfraction - yfraction) > EPSILON)
			return false;
		fraction = xdenum != 0 ? xfraction : yfraction;
	}
	double duration = (end1->t - start1->t);
	*t = start1->t + (long) (duration * fraction);
	return true;
}

/**
 * Returns true if the two segments of the temporal geographic point
 * values intersect at the timestamp
 *
 * @param[in] start1,end1 Temporal instants defining the first segment
 * @param[in] start2,end2 Temporal instants defining the second segment
 * @param[out] t Timestamp 
 * @pre The instants are synchronized, i.e., start1->t = start2->t and 
 * end1->t = end2->t
 */
bool
tgeogpointseq_intersection(const TInstant *start1, const TInstant *end1,
	const TInstant *start2, const TInstant *end2, TimestampTz *t)
{
	GEOGRAPHIC_EDGE e1, e2;
	POINT3D A1, A2, B1, B2;
	double fraction,
		xfraction = 0, yfraction = 0, zfraction = 0,
		xdenum, ydenum, zdenum;

	POINT4D p1 = datum_get_point4d(tinstant_value(start1));
	geographic_point_init(p1.x, p1.y, &(e1.start));
	geog2cart(&(e1.start), &A1);

	POINT4D p2 = datum_get_point4d(tinstant_value(end1));
	geographic_point_init(p2.x, p2.y, &(e1.end));
	geog2cart(&(e1.end), &A2);

	POINT4D p3 = datum_get_point4d(tinstant_value(start2));
	geographic_point_init(p3.x, p3.y, &(e2.start));
	geog2cart(&(e2.start), &B1);

	POINT4D p4 = datum_get_point4d(tinstant_value(end2));
	geographic_point_init(p4.x, p4.y, &(e2.end));
	geog2cart(&(e2.end), &B2);

	uint32_t inter = edge_intersects(&A1, &A2, &B1, &B2);
	if (inter == PIR_NO_INTERACT)
		return false;

	xdenum = A2.x - A1.x - B2.x + B1.x;
	ydenum = A2.y - A1.y - B2.y + B1.y;
	zdenum = A2.z - A1.z - B2.z + B1.z;
	if (xdenum == 0 && ydenum == 0 && zdenum == 0)
		/* Parallel segments */
		return false;

	if (xdenum != 0)
	{
		xfraction = (B1.x - A1.x) / xdenum;
		/* If intersection occurs out of the period */
		if (xfraction <= EPSILON || xfraction >= (1.0 - EPSILON))
			return false;
	}
	if (ydenum != 0)
	{
		yfraction = (B1.y - A1.y) / ydenum;
		/* If intersection occurs out of the period */
		if (yfraction <= EPSILON || yfraction >= (1.0 - EPSILON))
			return false;
	}
	if (zdenum != 0)
	{
		/* If intersection occurs out of the period or intersect at different timestamps */
		zfraction = (B1.z - A1.z) / zdenum;
		if (zfraction <= EPSILON || zfraction >= (1.0 - EPSILON))
			return false;
	}
	/* If intersect at different timestamps on each dimension
	 * We average the fractions found to limit floating point imprecision */
	if (xdenum != 0 && ydenum != 0 && zdenum != 0 &&
		fabsl(xfraction - yfraction) <= EPSILON && fabsl(xfraction - zfraction) <= EPSILON)
		fraction = (xfraction + yfraction + zfraction) / 3.0;
	else if (xdenum == 0 && ydenum != 0 && zdenum != 0 &&
		fabsl(yfraction - zfraction) <= EPSILON)
		fraction = (yfraction + zfraction) / 2.0;
	else if (xdenum != 0 && ydenum == 0 && zdenum != 0 &&
		fabsl(xfraction - zfraction) <= EPSILON)
		fraction = (xfraction + zfraction) / 2.0;
	else if (xdenum != 0 && ydenum != 0 && zdenum == 0 &&
		fabsl(xfraction - yfraction) <= EPSILON)
		fraction = (xfraction + yfraction) / 2.0;
	else if (xdenum != 0)
		fraction = xfraction;
	else if (ydenum != 0)
		fraction = yfraction;
	else if (zdenum != 0)
		fraction = zfraction;
	else
		return false;

	long double duration = (end1->t - start1->t);
	*t = start1->t + (long) (duration * fraction);
	return true;
}

/**
 * Returns true if the two segments of the temporal values
 * intersect at the timestamp
 *
 * @param[in] start1,end1 Temporal instants defining the first segment
 * @param[in] linear1 True when the interpolation of the first segment
 * is linear
 * @param[in] start2,end2 Temporal instants defining the second segment
 * @param[in] linear2 True when the interpolation of the second segment
 * is linear
 * @param[out] inter1, inter2 Base values taken by the two segments 
 * at the timestamp
 * @param[out] t Timestamp 
 * @pre The instants are synchronized, i.e., start1->t = start2->t and 
 * end1->t = end2->t
 */
bool
tsequence_intersection(const TInstant *start1, const TInstant *end1, bool linear1,
	const TInstant *start2, const TInstant *end2, bool linear2,
	Datum *inter1, Datum *inter2, TimestampTz *t)
{
	bool result = false; /* Make compiler quiet */
	if (! linear1)
	{
		*inter1 = tinstant_value(start1);
		result = tlinearseq_intersection_value(start2, end2,
			*inter1, start1->valuetypid, inter2, t);
	}
	else if (! linear2)
	{
		*inter2 = tinstant_value(start2);
		result = tlinearseq_intersection_value(start1, end1,
			*inter2, start2->valuetypid, inter1, t);
	}
	else
	{
		/* Both segments have linear interpolation */
		ensure_temporal_base_type(start1->valuetypid);
		if (numeric_base_type(start1->valuetypid))
			result = tnumberseq_intersection(start1, end1, start2, end2, t);
		else if (start1->valuetypid == type_oid(T_GEOMETRY))
			result = tgeompointseq_intersection(start1, end1, start2, end2, t);
		else if (start1->valuetypid == type_oid(T_GEOGRAPHY))
			result = tgeogpointseq_intersection(start1, end1, start2, end2, t);
		/* We are sure it is linear interpolation */
		if (result && inter1 != NULL)
			*inter1 = tsequence_value_at_timestamp1(start1, end1, true, *t);
		if (result && inter2 != NULL)
			*inter2 = tsequence_value_at_timestamp1(start2, end2, true, *t);
	}
	return result;
}

/*****************************************************************************
 * Are the three temporal instant values collinear?
 * These functions suppose that the segments are not constant.
 *****************************************************************************/

/**
 * Returns true if the three values are collinear
 * 
 * @param[in] x1,x2,x3 Input values
 * @param[in] ratio Value in [0,1] representing the duration of the 
 * timestamps associated to `x1` and `x2` divided by the duration
 * of the timestamps associated to `x1` and `x3`
 */
static bool
float_collinear(double x1, double x2, double x3, double ratio)
{
	double x = x1 + (x3 - x1) * ratio;
	return (fabs(x2 - x) <= EPSILON);
}

/**
 * Returns true if the three double2 values are collinear
 *
 * @param[in] x1,x2,x3 Input values
 * @param[in] ratio Value in [0,1] representing the duration of the 
 * timestamps associated to `x1` and `x2` divided by the duration
 * of the timestamps associated to `x1` and `x3`
 */
static bool
double2_collinear(const double2 *x1, const double2 *x2, const double2 *x3,
	double ratio)
{
	double2 x;
	x.a = x1->a + (x3->a - x1->a) * ratio;
	x.b = x1->b + (x3->b - x1->b) * ratio;
	bool result = (fabs(x2->a - x.a) <= EPSILON && fabs(x2->b - x.b) <= EPSILON);
	return result;
}

/**
 * Returns true if the three values are collinear
 *
 * @param[in] value1,value2,value3 Input values
 * @param[in] ratio Value in [0,1] representing the duration of the 
 * timestamps associated to `value1` and `value2` divided by the duration
 * of the timestamps associated to `value1` and `value3`
 * @param[in] hasz True when the points have Z coordinates
 */
static bool
geompoint_collinear(Datum value1, Datum value2, Datum value3,
	double ratio, bool hasz)
{
	POINT4D p1 = datum_get_point4d(value1);
	POINT4D p2 = datum_get_point4d(value2);
	POINT4D p3 = datum_get_point4d(value3);
	POINT4D p;
	interpolate_point4d(&p1, &p3, &p, ratio);
	bool result = hasz ?
		fabs(p2.x - p.x) <= EPSILON && fabs(p2.y - p.y) <= EPSILON &&
			fabs(p2.z - p.z) <= EPSILON :
		fabs(p2.x - p.x) <= EPSILON && fabs(p2.y - p.y) <= EPSILON;
	return result;
}

/**
 * Returns true if the three values are collinear
 *
 * @param[in] value1,value2,value3 Input values
 * @param[in] ratio Value in [0,1] representing the duration of the 
 * timestamps associated to `x1` and `x2` divided by the duration 
 * of the timestamps associated to `x1` and `x3`
 * @param[in] hasz True when the points have Z coordinates
 */
static bool
geogpoint_collinear(Datum value1, Datum value2, Datum value3,
	double ratio, bool hasz)
{
	Datum value = geogseg_interpolate_point(value1, value3, ratio);
	POINT4D p2 = datum_get_point4d(value2);
	POINT4D p = datum_get_point4d(value);
	bool result = hasz ?
		fabs(p2.x - p.x) <= EPSILON && fabs(p2.y - p.y) <= EPSILON &&
			fabs(p2.z - p.z) <= EPSILON :
		fabs(p2.x - p.x) <= EPSILON && fabs(p2.y - p.y) <= EPSILON;
	return result;
}

/**
 * Returns true if the three values are collinear
 *
 * @param[in] x1,x2,x3 Input values
 * @param[in] ratio Value in [0,1] representing the duration of the 
 * timestamps associated to `x1` and `x2` divided by the duration
 * of the timestamps associated to `x1` and `x3`
 */
static bool
double3_collinear(const double3 *x1, const double3 *x2, const double3 *x3,
	double ratio)
{
	double3 x;
	x.a = x1->a + (x3->a - x1->a) * ratio;
	x.b = x1->b + (x3->b - x1->b) * ratio,
	x.c = x1->c + (x3->c - x1->c) * ratio;
	bool result = (fabs(x2->a - x.a) <= EPSILON && 
		fabs(x2->b - x.b) <= EPSILON && fabs(x2->c - x.c) <= EPSILON);
	return result;
}

/**
 * Returns true if the three values are collinear
 *
 * @param[in] x1,x2,x3 Input values
 * @param[in] ratio Value in [0,1] representing the duration of the 
 * timestamps associated to `x1` and `x2` divided by the duration
 * of the timestamps associated to `x1` and `x3`
 */
static bool
double4_collinear(const double4 *x1, const double4 *x2, const double4 *x3,
	double ratio)
{
	double4 x;
	x.a = x1->a + (x3->a - x1->a) * ratio;
	x.b = x1->b + (x3->b - x1->b) * ratio;
	x.c = x1->c + (x3->c - x1->c) * ratio;
	x.d = x1->d + (x3->d - x1->d) * ratio;
	bool result = (fabs(x2->a - x.a) <= EPSILON && fabs(x2->b - x.b) <= EPSILON &&
		fabs(x2->c - x.c) <= EPSILON && fabs(x2->d - x.d) <= EPSILON);
	return result;
}

/**
 * Returns true if the three values are collinear
 *
 * @param[in] valuetypid Oid of the base type
 * @param[in] value1,value2,value3 Input values
 * @param[in] t1,t2,t3 Input timestamps
 */
static bool
datum_collinear(Oid valuetypid, Datum value1, Datum value2, Datum value3,
	TimestampTz t1, TimestampTz t2, TimestampTz t3)
{
	double duration1 = (double) (t2 - t1);
	double duration2 = (double) (t3 - t1);
	double ratio = duration1 / duration2;
	if (valuetypid == FLOAT8OID)
		return float_collinear(DatumGetFloat8(value1), DatumGetFloat8(value2), 
			DatumGetFloat8(value3), ratio);
	if (valuetypid == type_oid(T_DOUBLE2))
		return double2_collinear(DatumGetDouble2P(value1), DatumGetDouble2P(value2), 
			DatumGetDouble2P(value3), ratio);
	if (valuetypid == type_oid(T_GEOMETRY))
	{
		GSERIALIZED *gs = (GSERIALIZED *)DatumGetPointer(value1);
		bool hasz = (bool) FLAGS_GET_Z(gs->flags);
		return geompoint_collinear(value1, value2, value3, ratio, hasz);
	}
	if (valuetypid == type_oid(T_GEOGRAPHY))
	{
		GSERIALIZED *gs = (GSERIALIZED *)DatumGetPointer(value1);
		bool hasz = (bool) FLAGS_GET_Z(gs->flags);
		return geogpoint_collinear(value1, value2, value3, ratio, hasz);
	}
	if (valuetypid == type_oid(T_DOUBLE3))
		return double3_collinear(DatumGetDouble3P(value1), DatumGetDouble3P(value2), 
			DatumGetDouble3P(value3), ratio);
	if (valuetypid == type_oid(T_DOUBLE4))
		return double4_collinear(DatumGetDouble4P(value1), DatumGetDouble4P(value2), 
			DatumGetDouble4P(value3), ratio);
	return false;
}

/*****************************************************************************
 * Normalization functions
 *****************************************************************************/

/**
 * Normalize the array of temporal instant values
 *
 * @param[in] instants Array of input instants
 * @param[in] linear True when the instants have linear interpolation
 * @param[in] count Number of elements in the input array
 * @param[out] newcount Number of elements in the output array
 * @result Array of normalized temporal instant values
 * @pre The input array has at least two elements
 * @note The function does not create new instants, it creates an array of 
 * pointers to a subset of the input instants 
 */
static TInstant **
tinstantarr_normalize(TInstant **instants, bool linear, int count, 
	int *newcount)
{
	assert(count > 1);
	Oid valuetypid = instants[0]->valuetypid;
	TInstant **result = palloc(sizeof(TInstant *) * count);
	/* Remove redundant instants */ 
	TInstant *inst1 = instants[0];
	Datum value1 = tinstant_value(inst1);
	TInstant *inst2 = instants[1];
	Datum value2 = tinstant_value(inst2);
	result[0] = inst1;
	int k = 1;
	for (int i = 2; i < count; i++)
	{
		TInstant *inst3 = instants[i];
		Datum value3 = tinstant_value(inst3);
		if (
			/* step sequences and 2 consecutive instants that have the same value
				... 1@t1, 1@t2, 2@t3, ... -> ... 1@t1, 2@t3, ...
			*/
			(!linear && datum_eq(value1, value2, valuetypid))
			||
			/* 3 consecutive linear instants that have the same value
				... 1@t1, 1@t2, 1@t3, ... -> ... 1@t1, 1@t3, ...
			*/
			(linear && datum_eq(value1, value2, valuetypid) && datum_eq(value2, value3, valuetypid))
			||
			/* collinear linear instants
				... 1@t1, 2@t2, 3@t3, ... -> ... 1@t1, 3@t3, ...
			*/
			(linear && datum_collinear(valuetypid, value1, value2, value3, inst1->t, inst2->t, inst3->t))
			)
		{
			inst2 = inst3; value2 = value3;
		} 
		else 
		{
			result[k++] = inst2;
			inst1 = inst2; value1 = value2;
			inst2 = inst3; value2 = value3;
		}
	}
	result[k++] = inst2;
	*newcount = k;
	return result;
}

/**
 * Normalize the array of temporal sequence values
 *
 * The inpuy sequences may be non-contiguous but must ordered and
 * either (1) are non-overlapping, or (2) share the same last/first
 * instant in the case we are merging temporal sequences. 
 *
 * @param[in] sequences Array of input sequences
 * @param[in] count Number of elements in the input array
 * @param[out] newcount Number of elements in the output array
 * @result Array of normalized temporal sequences values
 * @pre Each sequence in the input array is normalized
 * @pre When merging sequences, the test whether the value is the same 
 * at the common instant should be ensured by the calling function.
 * @note The function creates new sequences and does not free the original
 * sequences
 */
TSequence **
tsequencearr_normalize(TSequence **sequences, int count, int *newcount)
{
	TSequence **result = palloc(sizeof(TSequence *) * count);
	/* seq1 is the sequence to which we try to join subsequent seq2 */
	TSequence *seq1 = sequences[0];
	Oid valuetypid = seq1->valuetypid;
	bool linear = MOBDB_FLAGS_GET_LINEAR(seq1->flags);
	bool isnew = false;
	int k = 0;
	for (int i = 1; i < count; i++)
	{
		TSequence *seq2 = sequences[i];
		TInstant *last2 = (seq1->count == 1) ? NULL : 
			tsequence_inst_n(seq1, seq1->count - 2); 
		Datum last2value = (seq1->count == 1) ? 0 : 
			tinstant_value(last2);
		TInstant *last1 = tsequence_inst_n(seq1, seq1->count - 1);
		Datum last1value = tinstant_value(last1);
		TInstant *first1 = tsequence_inst_n(seq2, 0);
		Datum first1value = tinstant_value(first1);
		TInstant *first2 = (seq2->count == 1) ? NULL : 
			tsequence_inst_n(seq2, 1); 
		Datum first2value = (seq2->count == 1) ? 0 : 
			tinstant_value(first2);
		bool adjacent = seq1->period.upper == seq2->period.lower &&
			(seq1->period.upper_inc || seq2->period.lower_inc);
		/* If they are adjacent and not instantaneous */
		if (adjacent && last2 != NULL && first2 != NULL &&
			(
			/* If step and the last segment of the first sequence is constant
			   ..., 1@t1, 1@t2) [1@t2, 1@t3, ... -> ..., 1@t1, 2@t3, ... 
			   ..., 1@t1, 1@t2) [1@t2, 2@t3, ... -> ..., 1@t1, 2@t3, ... 
			   ..., 1@t1, 1@t2] (1@t2, 2@t3, ... -> ..., 1@t1, 2@t3, ... 
			 */
			(!linear && 
			datum_eq(last2value, last1value, valuetypid) && 
			datum_eq(last1value, first1value, valuetypid))
			||			
			/* If the last/first segments are constant and equal 
			   ..., 1@t1, 1@t2] (1@t2, 1@t3, ... -> ..., 1@t1, 1@t3, ... 
			 */
			(datum_eq(last2value, last1value, valuetypid) &&
			datum_eq(last1value, first1value, valuetypid) && 
			datum_eq(first1value, first2value, valuetypid))
			||			
			/* If float/point sequences and collinear last/first segments having the same duration 
			   ..., 1@t1, 2@t2) [2@t2, 3@t3, ... -> ..., 1@t1, 3@t3, ... 
			*/
			(datum_eq(last1value, first1value, valuetypid) && 
			datum_collinear(valuetypid, last2value, first1value, first2value,
				last2->t, first1->t, first2->t))
			))
		{
			/* Remove the last and first instants of the sequences */
			seq1 = tsequence_join(seq1, seq2, true, true);
			isnew = true;
		}
		/* If step sequences and the first one has an exclusive upper bound,
		   by definition the first sequence has the last segment constant
		   ..., 1@t1, 1@t2) [2@t2, 3@t3, ... -> ..., 1@t1, 2@t2, 3@t3, ... 
		   ..., 1@t1, 1@t2) [2@t2] -> ..., 1@t1, 2@t2]
		 */
		else if (adjacent && !linear && !seq1->period.upper_inc)
		{
			/* Remove the last instant of the first sequence */
			seq1 = tsequence_join(seq1, seq2, true, false);
			isnew = true;
		}
		/* If they are adjacent and have equal last/first value respectively 
			Stewise
			... 1@t1, 2@t2], (2@t2, 1@t3, ... -> ..., 1@t1, 2@t2, 1@t3, ...
			[1@t1], (1@t1, 2@t2, ... -> ..., 1@t1, 2@t2
			Linear	
			..., 1@t1, 2@t2), [2@t2, 1@t3, ... -> ..., 1@t1, 2@t2, 1@t3, ...
			..., 1@t1, 2@t2], (2@t2, 1@t3, ... -> ..., 1@t1, 2@t2, 1@t3, ...
			..., 1@t1, 2@t2), [2@t2] -> ..., 1@t1, 2@t2]
			[1@t1],(1@t1, 2@t2, ... -> [1@t1, 2@t2, ...
		*/
		else if (adjacent && datum_eq(last1value, first1value, valuetypid))
		{
			/* Remove the first instant of the second sequence */
			seq1 = tsequence_join(seq1, seq2, false, true);
			isnew = true;
		} 
		else 
		{
			result[k++] = isnew ? seq1 : tsequence_copy(seq1);
			seq1 = seq2;
			isnew = false;
		}
	}
	result[k++] = isnew ? seq1 : tsequence_copy(seq1);
	*newcount = k;
	return result;
}

/*****************************************************************************/

/**
 * Returns the n-th instant of the temporal value
 */
TInstant *
tsequence_inst_n(const TSequence *seq, int index)
{
	return (TInstant *)(
		(char *)(&seq->offsets[seq->count + 2]) + 	/* start of data */
			seq->offsets[index]);					/* offset */
}

/**
 * Returns a pointer to the precomputed bounding box of the temporal value
 */
void *
tsequence_bbox_ptr(const TSequence *seq)
{
	return (char *)(&seq->offsets[seq->count + 2]) +  	/* start of data */
		seq->offsets[seq->count];						/* offset */
}

/**
 * Copy in the first argument the bounding box of the temporal value
 */
void
tsequence_bbox(void *box, const TSequence *seq)
{
	void *box1 = tsequence_bbox_ptr(seq);
	size_t bboxsize = temporal_bbox_size(seq->valuetypid);
	memcpy(box, box1, bboxsize);
}

/**
 * Construct a temporal sequence value from the array of temporal
 * instant values
 *
 * For example, the memory structure of a temporal sequence value with
 * 2 instants and a precomputed trajectory is as follows:
 * @code
 * -------------------------------------------------------------------
 * ( TSequence )_X | offset_0 | offset_1 | offset_2 | offset_3 | ...
 * -------------------------------------------------------------------
 * ------------------------------------------------------------------------
 * ( TInstant_0 )_X | ( TInstant_1 )_X | ( bbox )_X | ( Traj )_X  |
 * ------------------------------------------------------------------------
 * @endcode
 * where the `X` are unused bytes added for double padding, `offset_0` and
 * `offset_1` are offsets for the corresponding instants, `offset_2` is the
 * offset for the bounding box and `offset_3` is the offset for the 
 * precomputed trajectory. Precomputed trajectories are only kept for temporal
 * points of sequence duration.
 *
 * @param[in] instants Array of instants
 * @param[in] count Number of elements in the array
 * @param[in] lower_inc,upper_inc True when the respective bound is inclusive
 * @param[in] linear True when the interpolation is linear
 * @param[in] normalize True when the resulting value should be normalized
 */
TSequence *
tsequence_make(TInstant **instants, int count, bool lower_inc,
   bool upper_inc, bool linear, bool normalize)
{
	/* Test the validity of the instants */
	assert(count > 0);
	bool isgeo = point_base_type(instants[0]->valuetypid);
	ensure_valid_tinstantarr(instants, count, isgeo);
	if (count == 1 && (!lower_inc || !upper_inc))
		ereport(ERROR, (errcode(ERRCODE_RESTRICT_VIOLATION),
				errmsg("Instant sequence must have inclusive bounds")));
	if (!linear && count > 1 && !upper_inc &&
		datum_ne(tinstant_value(instants[count - 1]), 
			tinstant_value(instants[count - 2]), instants[0]->valuetypid))
		ereport(ERROR, (errcode(ERRCODE_RESTRICT_VIOLATION), 
			errmsg("Invalid end value for temporal sequence")));

	/* Normalize the array of instants */
	TInstant **norminsts = instants;
	int newcount = count;
	if (normalize && count > 1)
		norminsts = tinstantarr_normalize(instants, linear, count, &newcount);

	/* Get the bounding box size */
	size_t bboxsize = temporal_bbox_size(instants[0]->valuetypid);
	size_t memsize = double_pad(bboxsize);
	/* Add the size of composing instants */
	for (int i = 0; i < newcount; i++)
		memsize += double_pad(VARSIZE(norminsts[i]));
	/* Precompute the trajectory */
	bool hastraj = false; /* keep compiler quiet */
	Datum traj = 0; /* keep compiler quiet */
	if (isgeo)
	{
		hastraj = type_has_precomputed_trajectory(instants[0]->valuetypid);
		if (hastraj)
		{
			/* A trajectory is a geometry/geography, a point, a multipoint, 
			 * or a linestring, which may be self-intersecting */
			traj = tpointseq_make_trajectory(norminsts, newcount, linear);
			memsize += double_pad(VARSIZE(DatumGetPointer(traj)));
		}
	}
	/* Add the size of the struct and the offset array
	 * Notice that the first offset is already declared in the struct */
	size_t pdata = double_pad(sizeof(TSequence)) + (newcount + 1) * sizeof(size_t);
	/* Create the TSequence */
	TSequence *result = palloc0(pdata + memsize);
	SET_VARSIZE(result, pdata + memsize);
	result->count = newcount;
	result->valuetypid = instants[0]->valuetypid;
	result->duration = SEQUENCE;
	period_set(&result->period, norminsts[0]->t, norminsts[newcount - 1]->t,
		lower_inc, upper_inc);
	MOBDB_FLAGS_SET_LINEAR(result->flags, linear);
	MOBDB_FLAGS_SET_X(result->flags, true);
	MOBDB_FLAGS_SET_T(result->flags, true);
	if (isgeo)
	{
		MOBDB_FLAGS_SET_Z(result->flags, MOBDB_FLAGS_GET_Z(instants[0]->flags));
		MOBDB_FLAGS_SET_GEODETIC(result->flags, MOBDB_FLAGS_GET_GEODETIC(instants[0]->flags));
	}
	/* Initialization of the variable-length part */
	size_t pos = 0;
	for (int i = 0; i < newcount; i++)
	{
		memcpy(((char *)result) + pdata + pos, norminsts[i], 
			VARSIZE(norminsts[i]));
		result->offsets[i] = pos;
		pos += double_pad(VARSIZE(norminsts[i]));
	}
	/*
	 * Precompute the bounding box 
	 * Only external types have precomputed bounding box, internal types such
	 * as double2, double3, or double4 do not have precomputed bounding box.
	 * For temporal points the bounding box is computed from the trajectory 
	 * for efficiency reasons.
	 */
	if (bboxsize != 0)
	{
		void *bbox = ((char *) result) + pdata + pos;
		if (hastraj)
		{
			geo_to_stbox_internal(bbox, (GSERIALIZED *)DatumGetPointer(traj));
			((STBOX *)bbox)->tmin = result->period.lower;
			((STBOX *)bbox)->tmax = result->period.upper;
			MOBDB_FLAGS_SET_T(((STBOX *)bbox)->flags, true);
		}
		else
			tsequence_make_bbox(bbox, norminsts, newcount,
				lower_inc, upper_inc);
		result->offsets[newcount] = pos;
		pos += double_pad(bboxsize);
	}
	if (isgeo && hastraj)
	{
		result->offsets[newcount + 1] = pos;
		memcpy(((char *) result) + pdata + pos, DatumGetPointer(traj),
			VARSIZE(DatumGetPointer(traj)));
		pfree(DatumGetPointer(traj));
	}

	if (normalize && count > 2)
		pfree(norminsts);

	return result;
}

/**
 * Construct a temporal sequence value from the array of temporal
 * instant values and free the array and the instants after the creation
 *
 * @param[in] instants Array of instants
 * @param[in] count Number of elements in the array
 * @param[in] lower_inc,upper_inc True when the respective bound is inclusive
 * @param[in] linear True when the interpolation is linear
 * @param[in] normalize True when the resulting value should be normalized
 */
TSequence *
tsequence_make_free(TInstant **instants, int count, bool lower_inc,
   bool upper_inc, bool linear, bool normalize)
{
	assert (count > 0);
	TSequence *result = tsequence_make(instants, count, lower_inc, upper_inc,
		linear, normalize);
	for (int i = 0; i < count; i++)
		pfree(instants[i]);
	pfree(instants);
	return result;
}

/**
 * Join the two temporal sequence values
 *
 * @param[in] seq1,seq2 Temporal sequence values
 * @param[in] removelast,removefirst Remove the last and/or the 
 * first instant of the first/second sequence
 * @pre The two input sequences are adjacent and have the same interpolation
 * @note The function is called when normalizing an array of sequences 
 */
TSequence *
tsequence_join(const TSequence *seq1, const TSequence *seq2,
	bool removelast, bool removefirst)
{
	/* Ensure that the two sequences has the same interpolation */
	assert(MOBDB_FLAGS_GET_LINEAR(seq1->flags) == 
		MOBDB_FLAGS_GET_LINEAR(seq2->flags));
	Oid valuetypid = seq1->valuetypid;

	size_t bboxsize = temporal_bbox_size(valuetypid);
	size_t memsize = double_pad(bboxsize);

	int count1 = removelast ? seq1->count - 1 : seq1->count;
	int start2 = removefirst ? 1 : 0;
	for (int i = 0; i < count1; i++)
		memsize += double_pad(VARSIZE(tsequence_inst_n(seq1, i)));
	for (int i = start2; i < seq2->count; i++)
		memsize += double_pad(VARSIZE(tsequence_inst_n(seq2, i)));

	int count = count1 + (seq2->count - start2);

	bool hastraj = type_has_precomputed_trajectory(valuetypid);
	Datum traj = 0; /* keep compiler quiet */
	if (hastraj)
	{
		/* A trajectory is a geometry/geography, either a point or a
		 * linestring, which may be self-intersecting */
		traj = tpointseq_trajectory_join(seq1, seq2, removelast, removefirst);
		memsize += double_pad(VARSIZE(DatumGetPointer(traj)));
	}

	/* Add the size of the struct and the offset array
	 * Notice that the first offset is already declared in the struct */
	size_t pdata = double_pad(sizeof(TSequence)) + (count + 1) * sizeof(size_t);
	/* Create the TSequence */
	TSequence *result = palloc0(pdata + memsize);
	SET_VARSIZE(result, pdata + memsize);
	result->count = count;
	result->valuetypid = valuetypid;
	result->duration = SEQUENCE;
	period_set(&result->period, seq1->period.lower, seq2->period.upper,
		seq1->period.lower_inc, seq2->period.upper_inc);
	MOBDB_FLAGS_SET_LINEAR(result->flags, MOBDB_FLAGS_GET_LINEAR(seq1->flags));
	MOBDB_FLAGS_SET_X(result->flags, true);
	MOBDB_FLAGS_SET_T(result->flags, true);
	if (point_base_type(valuetypid))
	{
		MOBDB_FLAGS_SET_Z(result->flags, MOBDB_FLAGS_GET_Z(seq1->flags));
		MOBDB_FLAGS_SET_GEODETIC(result->flags, MOBDB_FLAGS_GET_GEODETIC(seq1->flags));
	}

	/* Initialization of the variable-length part */
	int k = 0;
	size_t pos = 0;
	for (int i = 0; i < count1; i++)
	{
		memcpy(((char *)result) + pdata + pos, tsequence_inst_n(seq1, i),
			VARSIZE(tsequence_inst_n(seq1, i)));
		result->offsets[k++] = pos;
		pos += double_pad(VARSIZE(tsequence_inst_n(seq1, i)));
	}
	for (int i = start2; i < seq2->count; i++)
	{
		memcpy(((char *)result) + pdata + pos, tsequence_inst_n(seq2, i),
			VARSIZE(tsequence_inst_n(seq2, i)));
		result->offsets[k++] = pos;
		pos += double_pad(VARSIZE(tsequence_inst_n(seq2, i)));
	}
	/*
	 * Precompute the bounding box
	 */
	if (bboxsize != 0)
	{
		void *bbox = ((char *) result) + pdata + pos;
		if (valuetypid == BOOLOID || valuetypid == TEXTOID)
			memcpy(bbox, &result->period, bboxsize);
		else
		{
			memcpy(bbox, tsequence_bbox_ptr(seq1), bboxsize);
			temporal_bbox_expand(bbox, tsequence_bbox_ptr(seq2), valuetypid);
		}
		result->offsets[k] = pos;
		pos += double_pad(bboxsize);
	}
	if (hastraj)
	{
		result->offsets[k + 1] = pos;
		memcpy(((char *) result) + pdata + pos, DatumGetPointer(traj),
			VARSIZE(DatumGetPointer(traj)));
		pfree(DatumGetPointer(traj));
	}

	return result;
}

/**
 * Construct a temporal sequence value from a base value and a period
 * (internal function)
 *
 * @param[in] value Base value
 * @param[in] valuetypid Oid of the base type
 * @param[in] p Period
 * @param[in] linear True when the resulting value has linear interpolation
 */
TSequence *
tsequence_from_base_internal(Datum value, Oid valuetypid, const Period *p,
	bool linear)
{
	TInstant *instants[2];
	instants[0] = tinstant_make(value, p->lower, valuetypid);
	instants[1] = tinstant_make(value, p->upper, valuetypid);
	TSequence *result = tsequence_make(instants, 2, p->lower_inc,
		p->upper_inc, linear, NORMALIZE_NO);
	pfree(instants[0]); pfree(instants[1]);
	return result;
}

PG_FUNCTION_INFO_V1(tsequence_from_base);
/**
 * Construct a temporal sequence value from a base value and a period
 */
PGDLLEXPORT Datum
tsequence_from_base(PG_FUNCTION_ARGS)
{
	Datum value = PG_GETARG_ANYDATUM(0);
	Period *p = PG_GETARG_PERIOD(1);
	bool linear = PG_GETARG_BOOL(2);
	Oid valuetypid = get_fn_expr_argtype(fcinfo->flinfo, 0);
	TSequence *result = tsequence_from_base_internal(value, valuetypid, p, linear);
	DATUM_FREE_IF_COPY(value, valuetypid, 0);
	PG_RETURN_POINTER(result);
}

/**
 * Append an instant to the temporal value
 */
TSequence *
tsequence_append_tinstant(const TSequence *seq, const TInstant *inst)
{
	/* Test the validity of the instant */
	assert(seq->valuetypid == inst->valuetypid);
	TInstant *inst1 = tsequence_inst_n(seq, seq->count - 1);
	ensure_increasing_timestamps(inst1, inst);
	bool isgeo = point_base_type(seq->valuetypid);
	if (isgeo)
	{
		ensure_same_geodetic_tpoint((Temporal *)seq, (Temporal *)inst);
		ensure_same_srid_tpoint((Temporal *)seq, (Temporal *)inst);
		ensure_same_dimensionality_tpoint((Temporal *)seq, (Temporal *)inst);
	}

	bool linear = MOBDB_FLAGS_GET_LINEAR(seq->flags);
	/* Normalize the result */
	int newcount = seq->count + 1;
	if (seq->count > 1)
	{
		inst1 = tsequence_inst_n(seq, seq->count - 2);
		Datum value1 = tinstant_value(inst1);
		TInstant *inst2 = tsequence_inst_n(seq, seq->count - 1);
		Datum value2 = tinstant_value(inst2);
		Datum value3 = tinstant_value(inst);
		if (
			/* step sequences and 2 consecutive instants that have the same value
				... 1@t1, 1@t2, 2@t3, ... -> ... 1@t1, 2@t3, ...
			*/
			(! linear && datum_eq(value1, value2, seq->valuetypid))
			||
			/* 3 consecutive float/point instants that have the same value 
				... 1@t1, 1@t2, 1@t3, ... -> ... 1@t1, 1@t3, ...
			*/
			(datum_eq(value1, value2, seq->valuetypid) && datum_eq(value2, value3, seq->valuetypid))
			||
			/* collinear float/point instants that have the same duration
				... 1@t1, 2@t2, 3@t3, ... -> ... 1@t1, 3@t3, ...
			*/
			(linear && datum_collinear(seq->valuetypid, value1, value2, value3, inst1->t, inst2->t, inst->t))
			)
		{
			/* The new instant replaces the last instant of the sequence */
			newcount--;
		} 
	}
	/* Get the bounding box size */
	size_t bboxsize = temporal_bbox_size(seq->valuetypid);
	size_t memsize = double_pad(bboxsize);
	/* Add the size of composing instants */
	for (int i = 0; i < newcount - 1; i++)
		memsize += double_pad(VARSIZE(tsequence_inst_n(seq, i)));
	memsize += double_pad(VARSIZE(inst));
	/* Expand the trajectory */
	bool hastraj = false; /* keep compiler quiet */
	Datum traj = 0; /* keep compiler quiet */
	if (isgeo)
	{
		hastraj = type_has_precomputed_trajectory(seq->valuetypid);
		if (hastraj)
		{
			bool replace = newcount != seq->count + 1;
			traj = tpointseq_trajectory_append(seq, inst, replace);
			memsize += double_pad(VARSIZE(DatumGetPointer(traj)));
		}
	}
	/* Add the size of the struct and the offset array
	 * Notice that the first offset is already declared in the struct */
	size_t pdata = double_pad(sizeof(TSequence)) + (newcount + 1) * sizeof(size_t);
	/* Create the TSequence */
	TSequence *result = palloc0(pdata + memsize);
	SET_VARSIZE(result, pdata + memsize);
	result->count = newcount;
	result->valuetypid = seq->valuetypid;
	result->duration = SEQUENCE;
	period_set(&result->period, seq->period.lower, inst->t, 
		seq->period.lower_inc, true);
	MOBDB_FLAGS_SET_LINEAR(result->flags, MOBDB_FLAGS_GET_LINEAR(seq->flags));
	MOBDB_FLAGS_SET_X(result->flags, true);
	MOBDB_FLAGS_SET_T(result->flags, true);
	if (isgeo)
	{
		MOBDB_FLAGS_SET_Z(result->flags, MOBDB_FLAGS_GET_Z(seq->flags));
		MOBDB_FLAGS_SET_GEODETIC(result->flags, MOBDB_FLAGS_GET_GEODETIC(seq->flags));
	}
	/* Initialization of the variable-length part */
	size_t pos = 0;
	for (int i = 0; i < newcount - 1; i++)
	{
		inst1 = tsequence_inst_n(seq, i);
		memcpy(((char *)result) + pdata + pos, inst1, VARSIZE(inst1));
		result->offsets[i] = pos;
		pos += double_pad(VARSIZE(inst1));
	}
	/* Append the instant */
	memcpy(((char *)result) + pdata + pos, inst, VARSIZE(inst));
	result->offsets[newcount - 1] = pos;
	pos += double_pad(VARSIZE(inst));
	/* Expand the bounding box */
	if (bboxsize != 0) 
	{
		bboxunion box;
		void *bbox = ((char *) result) + pdata + pos;
		memcpy(bbox, tsequence_bbox_ptr(seq), bboxsize);
		tinstant_make_bbox(&box, inst);
		temporal_bbox_expand(bbox, &box, seq->valuetypid);
		result->offsets[newcount] = pos;
	}
	if (isgeo && hastraj)
	{
		result->offsets[newcount + 1] = pos;
		memcpy(((char *) result) + pdata + pos, DatumGetPointer(traj),
			VARSIZE(DatumGetPointer(traj)));
		pfree(DatumGetPointer(traj));
	}
	return result;
}

/**
 * Merge the two temporal values
 */
Temporal *
tsequence_merge(const TSequence *seq1, const TSequence *seq2)
{
	const TSequence *sequences[] = {seq1, seq2};
	return tsequence_merge_array((TSequence **) sequences, 2);
}

/**
 * Merge the array of temporal sequence values. The function does not assume
 * that the values in the array can be strictly ordered on time, i.e., the
 * intersection of the bounding boxes of two values may be a period. 
 * For this reason two passes are necessary.
 *
 * @param[in] sequences Array of values
 * @param[in] count Number of elements in the array
 * @result Merged value
 */
Temporal *
tsequence_merge_array(TSequence **sequences, int count)
{
	/* Sort the array */
	if (count > 1)
		tsequencearr_sort(sequences, count);
	bool linear = MOBDB_FLAGS_GET_LINEAR(sequences[0]->flags);
	/* Test the validity of the temporal values */
	bool isgeo = point_base_type(sequences[0]->valuetypid);
	/* Number of instants in the resulting sequences */
	int *countinst = palloc0(sizeof(int) * count);
	/* Number of instants of the longest sequence */
	int maxcount = countinst[0] = sequences[0]->count;
	int k = 0; /* Number of resulting sequences */
	for (int i = 1; i < count; i++)
	{
		/* Test the validity of consecutive temporal values */
		ensure_same_interpolation((Temporal *)sequences[i - 1], (Temporal *)sequences[i]);
		if (isgeo)
		{
			ensure_same_geodetic_tpoint((Temporal *) sequences[i - 1], (Temporal *) sequences[i]);
			ensure_same_srid_tpoint((Temporal *) sequences[i - 1], (Temporal *) sequences[i]);
			ensure_same_dimensionality_tpoint((Temporal *) sequences[i - 1], (Temporal *) sequences[i]);
		}
		TInstant *inst1 = tsequence_inst_n(sequences[i - 1], sequences[i - 1]->count - 1);
		TInstant *inst2 = tsequence_inst_n(sequences[i], 0);
		if (inst1->t > inst2->t)
			ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("The temporal values cannot overlap on time")));
		if (inst1->t == inst2->t && sequences[i]->period.lower_inc)
		{
			if (! datum_eq(tinstant_value(inst1), tinstant_value(inst2), inst1->valuetypid) &&
				sequences[i - 1]->period.upper_inc && sequences[i]->period.lower_inc)
				ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
					errmsg("The temporal values have different value at their overlapping instant")));
			else
				/* Continue with the current sequence */
				countinst[k] += sequences[i]->count - 1;
		}
		else
		{
			/* Update the number of instants of the longest sequence */
			if (maxcount < countinst[k])
				maxcount = countinst[k];
			/* Start a new sequence */
			countinst[++k] = sequences[i]->count;
		}
	}
	if (maxcount < countinst[k])
		maxcount = countinst[k];
	k++;
	TSequence **newseqs = palloc(sizeof(TSequence *) * k);
	TInstant **instants = palloc(sizeof(TInstant *) * maxcount);
	int l = 0; /* Number of the current input sequence */
	for (int i = 0; i < k; i++)
	{
		bool lowerinc = sequences[l]->period.lower_inc;
		int m = 0; /* Number of instants of the current output sequence */
		while (m < countinst[i] && l < count)
		{
			int start = sequences[l]->period.lower_inc && ( m == 0 || !
				sequences[l - 1]->period.upper_inc ) ? 0 : 1;
			int end = sequences[l]->period.upper_inc ? sequences[l]->count :
				sequences[l]->count - 1;
			for (int j = start; j < end; j++)
				instants[m++] = tsequence_inst_n(sequences[l], j);
			l++;
		}
		bool upperinc = sequences[l - 1]->period.upper_inc;
		if (! upperinc)
			instants[m] = tsequence_inst_n(sequences[l - 1], sequences[l - 1]->count - 1);
		newseqs[i] = tsequence_make(instants, countinst[i], lowerinc,
			upperinc, linear, NORMALIZE);
	}
	pfree(instants);
	Temporal *result = (k == 1) ? (Temporal *) newseqs[0] :
		(Temporal *) tsequenceset_make_free(newseqs, k, NORMALIZE);
	pfree(countinst);
	return result;
}

/**
 * Returns a copy of the temporal value
 */
TSequence *
tsequence_copy(const TSequence *seq)
{
	TSequence *result = palloc0(VARSIZE(seq));
	memcpy(result, seq, VARSIZE(seq));
	return result;
}

/**
 * Returns the index of the segment of the temporal sequence value
 * containing the timestamp using binary search
 *
 * If the timestamp is contained in the temporal value, the index of the
 * segment containing the timestamp is returned in the output parameter. 
 * For example, given a value composed of 3 sequences and a timestamp, 
 * the value returned in the output parameter is as follows:
 * @code
 *            0     1     2     3
 *            |-----|-----|-----|   
 * 1)    t^                             => result = -1
 * 2)        t^                         => result = 0 if the lower bound is inclusive, -1 otherwise
 * 3)              t^                   => result = 1
 * 4)                 t^                => result = 1
 * 5)                             t^    => result = -1
 * @endcode
 *
 * @param[in] seq Temporal sequence value
 * @param[in] t Timestamp
 * @result Returns -1 if the timestamp is not contained in the temporal value
 */
int
tsequence_find_timestamp(const TSequence *seq, TimestampTz t)
{
	int first = 0;
	int last = seq->count - 2;
	int middle = (first + last)/2;
	while (first <= last) 
	{
		TInstant *inst1 = tsequence_inst_n(seq, middle);
		TInstant *inst2 = tsequence_inst_n(seq, middle + 1);
		bool lower_inc = (middle == 0) ? seq->period.lower_inc : true;
		bool upper_inc = (middle == seq->count - 2) ? seq->period.upper_inc : false;
		if ((inst1->t < t && t < inst2->t) ||
			(lower_inc && inst1->t == t) || (upper_inc && inst2->t == t))
			return middle;
		if (t <= inst1->t)
			last = middle - 1;
		else
			first = middle + 1;	
		middle = (first + last)/2;
	}
	return -1;
}

/**
 * Convert an an array of arrays of temporal sequence values into an array of
 * sequence values.
 *
 * @param[in] sequences Array of array of temporal sequence values
 * @param[in] countseqs Array of counters
 * @param[in] count Number of elements in the first dimension of the arrays
 * @param[in] totalseqs Number of elements in the output array
 */
TSequence **
tsequencearr2_to_tsequencearr(TSequence ***sequences, int *countseqs, 
	int count, int totalseqs)
{
	TSequence **result = palloc(sizeof(TSequence *) * totalseqs);
	int k = 0;
	for (int i = 0; i < count; i++)
	{
		for (int j = 0; j < countseqs[i]; j++)
			result[k++] = sequences[i][j];
		if (countseqs[i] != 0)
			pfree(sequences[i]);
	}
	pfree(sequences); pfree(countseqs);	
	return result;
}

/*****************************************************************************
 * Intersection functions
 *****************************************************************************/
 
/**
 * Temporally intersect the two temporal values
 *
 * @param[in] seq,inst Input values
 * @param[out] inter1, inter2 Output values
 * @result Returns false if the input values do not overlap on time.
 */
bool
intersection_tsequence_tinstant(const TSequence *seq, const TInstant *inst,
	TInstant **inter1, TInstant **inter2)
{
	TInstant *inst1 = tsequence_at_timestamp(seq, inst->t);
	if (inst1 == NULL)
		return false;
	
	*inter1 = inst1;
	*inter2 = tinstant_copy(inst1);
	return true;
}

/**
 * Temporally intersect the two temporal values
 *
 * @param[in] inst,seq Temporal values
 * @param[out] inter1, inter2 Output values
 * @result Returns false if the input values do not overlap on time.
 */
bool
intersection_tinstant_tsequence(const TInstant *inst, const TSequence *seq,
	TInstant **inter1, TInstant **inter2)
{
	return intersection_tsequence_tinstant(seq, inst, inter2, inter1);
}

/**
 * Temporally intersect the two temporal values
 *
 * @param[in] seq,ti Input values
 * @param[out] inter1, inter2 Output values
 * @result Returns false if the input values do not overlap on time.
 */
bool
intersection_tsequence_tinstantset(const TSequence *seq, const TInstantSet *ti,
	TInstantSet **inter1, TInstantSet **inter2)
{
	/* Test whether the bounding period of the two temporal values overlap */
	Period p;
	tinstantset_period(&p, ti);
	if (!overlaps_period_period_internal(&seq->period, &p))
		return false;
	
	TInstant **instants1 = palloc(sizeof(TInstant *) * ti->count);
	TInstant **instants2 = palloc(sizeof(TInstant *) * ti->count);
	int k = 0;
	for (int i = 0; i < ti->count; i++)
	{
		TInstant *inst = tinstantset_inst_n(ti, i);
		if (contains_period_timestamp_internal(&seq->period, inst->t))
		{
			instants1[k] = tsequence_at_timestamp(seq, inst->t);
			instants2[k++] = inst;
		}
		if (seq->period.upper < inst->t)
			break;
	}
	if (k == 0)
	{
		pfree(instants1); pfree(instants2); 
		return false;
	}
	
	*inter1 = tinstantset_make_free(instants1, k);
	*inter2 = tinstantset_make(instants2, k);
	pfree(instants2); 
	return true;
}

/**
 * Temporally intersect the two temporal values
 *
 * @param[in] ti,seq Temporal values
 * @param[out] inter1,inter2 Output values
 * @result Returns false if the input values do not overlap on time.
 */
bool
intersection_tinstantset_tsequence(const TInstantSet *ti, const TSequence *seq,
	TInstantSet **inter1, TInstantSet **inter2)
{
	return intersection_tsequence_tinstantset(seq, ti, inter2, inter1);
}

/**
 * Temporally intersect the two temporal values
 *
 * @param[in] seq1,seq2 Input values
 * @param[out] inter1, inter2 Output values
 * @result Returns false if the input values do not overlap on time.
 */
bool
intersection_tsequence_tsequence(const TSequence *seq1, const TSequence *seq2,
	TSequence **inter1, TSequence **inter2)
{
	/* Test whether the bounding period of the two temporal values overlap */
	Period *inter = intersection_period_period_internal(&seq1->period, 
		&seq2->period);
	if (inter == NULL)
		return false;
	
	*inter1 = tsequence_at_period(seq1, inter);
	*inter2 = tsequence_at_period(seq2, inter);
	pfree(inter);
	return true;
}

/*****************************************************************************
 * Synchronize two TSequence values. The values are split into (redundant)
 * segments defined over the same set of instants covering the intersection
 * of their time spans. Depending on the value of the argument crossings,
 * potential crossings between successive pair of instants are added.
 * Crossings are only added when at least one of the sequences has linear
 * interpolation.
 *****************************************************************************/

/**
 * Synchronize the two temporal values
 *
 * The resulting values are composed of a denormalized sequence
 * covering the intersection of their time spans
 *
 * @param[in] seq1,seq2 Input values
 * @param[out] sync1, sync2 Output values
 * @param[in] crossings State whether turning points are added in the segments
 * @result Returns false if the input values do not overlap on time
 */
bool
synchronize_tsequence_tsequence(const TSequence *seq1, const TSequence *seq2,
	TSequence **sync1, TSequence **sync2, bool crossings)
{
	/* Test whether the bounding period of the two temporal values overlap */
	Period *inter = intersection_period_period_internal(&seq1->period, 
		&seq2->period);
	if (inter == NULL)
		return false;

	bool linear1 = MOBDB_FLAGS_GET_LINEAR(seq1->flags);
	bool linear2 = MOBDB_FLAGS_GET_LINEAR(seq2->flags);
	/* If the two sequences intersect at an instant */
	if (inter->lower == inter->upper)
	{
		TInstant *inst1 = tsequence_at_timestamp(seq1, inter->lower);
		TInstant *inst2 = tsequence_at_timestamp(seq2, inter->lower);
		*sync1 = tsequence_make(&inst1, 1, true, true, linear1, NORMALIZE_NO);
		*sync2 = tsequence_make(&inst2, 1, true, true, linear2, NORMALIZE_NO);
		pfree(inst1); pfree(inst2); pfree(inter);
		return true;
	}
	
	/* 
	 * General case 
	 * seq1 =  ... *     *   *   *      *>
	 * seq2 =       <*            *     * ...
	 * sync1 =      <X C * C * C X C X C *>
	 * sync1 =      <* C X C X C * C * C X>
	 * where X are values added for synchronization and C are values added
	 * for the crossings
	 */
	TInstant *inst1 = tsequence_inst_n(seq1, 0);
	TInstant *inst2 = tsequence_inst_n(seq2, 0);
	TInstant *tofreeinst = NULL;
	int i = 0, j = 0, k = 0, l = 0;
	if (inst1->t < inter->lower)
	{
		inst1 = tofreeinst = tsequence_at_timestamp(seq1, inter->lower);
		i = tsequence_find_timestamp(seq1, inter->lower);
	}
	else if (inst2->t < inter->lower)
	{
		inst2 = tofreeinst = tsequence_at_timestamp(seq2, inter->lower);
		j = tsequence_find_timestamp(seq2, inter->lower);
	}
	int count = (seq1->count - i + seq2->count - j) * 2;
	TInstant **instants1 = palloc(sizeof(TInstant *) * count);
	TInstant **instants2 = palloc(sizeof(TInstant *) * count);
	TInstant **tofree = palloc(sizeof(TInstant *) * count * 2);
	if (tofreeinst != NULL)
		tofree[l++] = tofreeinst;
	while (i < seq1->count && j < seq2->count &&
		(inst1->t <= inter->upper || inst2->t <= inter->upper))
	{
		int cmp = timestamp_cmp_internal(inst1->t, inst2->t);
		if (cmp == 0)
		{
			i++; j++;
		}
		else if (cmp < 0)
		{
			i++;
			inst2 = tsequence_at_timestamp(seq2, inst1->t);
			tofree[l++] = inst2;
		}
		else 
		{
			j++;
			inst1 = tsequence_at_timestamp(seq1, inst2->t);
			tofree[l++] = inst1;
		}
		/* If not the first instant add potential crossing before adding
		   the new instants */
		if (crossings && (linear1 || linear2) && k > 0)
		{
			TimestampTz crosstime;
			Datum inter1, inter2;
			if (tsequence_intersection(instants1[k - 1], inst1, linear1,
				instants2[k - 1], inst2, linear2, &inter1, &inter2, &crosstime))
			{
				instants1[k] = tofree[l++] = tinstant_make(inter1,
					crosstime, seq1->valuetypid);
				instants2[k++] = tofree[l++] = tinstant_make(inter2,
					crosstime, seq2->valuetypid);
			}
		}
		instants1[k] = inst1; instants2[k++] = inst2;
		if (i == seq1->count || j == seq2->count)
			break;
		inst1 = tsequence_inst_n(seq1, i);
		inst2 = tsequence_inst_n(seq2, j);
	}
	/* We are sure that k != 0 due to the period intersection test above */
	/* The last two values of sequences with step interpolation and
	   exclusive upper bound must be equal */
	if (! inter->upper_inc && k > 1 && ! linear1)
	{
		if (datum_ne(tinstant_value(instants1[k - 2]), 
			tinstant_value(instants1[k - 1]), seq1->valuetypid))
		{
			instants1[k - 1] = tinstant_make(tinstant_value(instants1[k - 2]),
				instants1[k - 1]->t, instants1[k - 1]->valuetypid); 
			tofree[l++] = instants1[k - 1];
		}
	}
	if (! inter->upper_inc && k > 1 && ! linear2)
	{
		if (datum_ne(tinstant_value(instants2[k - 2]), 
			tinstant_value(instants2[k - 1]), seq2->valuetypid))
		{
			instants2[k - 1] = tinstant_make(tinstant_value(instants2[k - 2]),
				instants2[k - 1]->t, instants2[k - 1]->valuetypid); 
			tofree[l++] = instants2[k - 1];
		}
	}
	*sync1 = tsequence_make(instants1, k, inter->lower_inc,
		inter->upper_inc, linear1, NORMALIZE_NO);
	*sync2 = tsequence_make(instants2, k, inter->lower_inc,
		inter->upper_inc, linear2, NORMALIZE_NO);
	
	for (i = 0; i < l; i++)
		pfree(tofree[i]);
	pfree(instants1); pfree(instants2); pfree(tofree); pfree(inter);

	return true;
}

/*****************************************************************************
 * Input/output functions
 *****************************************************************************/

/**
 * Returns the string representation of the temporal value
 *
 * @param[in] seq Temporal value
 * @param[in] component True when the output string is a component of
 * a temporal sequence set value and thus no interpolation string 
 * at the begining of the string should be output
 * @param[in] value_out Function called to output the base value
 * depending on its Oid
 */
char *
tsequence_to_string(const TSequence *seq, bool component, 
	char *(*value_out)(Oid, Datum))
{
	char **strings = palloc(sizeof(char *) * seq->count);
	size_t outlen = 0;
	char prefix[20];
	if (! component && linear_interpolation(seq->valuetypid) && 
		!MOBDB_FLAGS_GET_LINEAR(seq->flags))
		sprintf(prefix, "Interp=Stepwise;");
	else
		prefix[0] = '\0';
	for (int i = 0; i < seq->count; i++)
	{
		TInstant *inst = tsequence_inst_n(seq, i);
		strings[i] = tinstant_to_string(inst, value_out);
		outlen += strlen(strings[i]) + 2;
	}
	char open = seq->period.lower_inc ? (char) '[' : (char) '(';
	char close = seq->period.upper_inc ? (char) ']' : (char) ')';
	return stringarr_to_string(strings, seq->count, outlen, prefix,
		open, close);
}

/**
 * Write the binary representation of the temporal value
 * into the buffer
 *
 * @param[in] seq Temporal value
 * @param[in] buf Buffer
 */
void
tsequence_write(const TSequence *seq, StringInfo buf)
{
#if MOBDB_PGSQL_VERSION < 110000
	pq_sendint(buf, (uint32) seq->count, 4);
#else
	pq_sendint32(buf, seq->count);
#endif
	pq_sendbyte(buf, seq->period.lower_inc ? (uint8) 1 : (uint8) 0);
	pq_sendbyte(buf, seq->period.upper_inc ? (uint8) 1 : (uint8) 0);
	pq_sendbyte(buf, MOBDB_FLAGS_GET_LINEAR(seq->flags) ? (uint8) 1 : (uint8) 0);
	for (int i = 0; i < seq->count; i++)
	{
		TInstant *inst = tsequence_inst_n(seq, i);
		tinstant_write(inst, buf);
	}
}

/**
 * Returns a new temporal value from its binary representation 
 * read from the buffer (dispatch function)
 *
 * @param[in] buf Buffer
 * @param[in] valuetypid Oid of the base type
 */
TSequence *
tsequence_read(StringInfo buf, Oid valuetypid)
{
	int count = (int) pq_getmsgint(buf, 4);
	bool lower_inc = (char) pq_getmsgbyte(buf);
	bool upper_inc = (char) pq_getmsgbyte(buf);
	bool linear = (char) pq_getmsgbyte(buf);
	TInstant **instants = palloc(sizeof(TInstant *) * count);
	for (int i = 0; i < count; i++)
		instants[i] = tinstant_read(buf, valuetypid);
	return tsequence_make_free(instants, count, lower_inc,
		upper_inc, linear, NORMALIZE);
}

/*****************************************************************************
 * Cast functions
 *****************************************************************************/

/**
 * Cast the temporal integer value as a temporal float value
 */
TSequence *
tintseq_to_tfloatseq(const TSequence *seq)
{
	/* It is not necessary to set the linear flag to false since it is already
	 * set by the fact that the input argument is a temporal integer */
	TSequence *result = tsequence_copy(seq);
	result->valuetypid = FLOAT8OID;
	for (int i = 0; i < seq->count; i++)
	{
		TInstant *inst = tsequence_inst_n(result, i);
		inst->valuetypid = FLOAT8OID;
		Datum *value_ptr = tinstant_value_ptr(inst);
		*value_ptr = Float8GetDatum((double)DatumGetInt32(tinstant_value(inst)));
	}
	return result;
}

/**
 * Cast the temporal float value as a temporal integer value
 */
TSequence *
tfloatseq_to_tintseq(const TSequence *seq)
{
	if (MOBDB_FLAGS_GET_LINEAR(seq->flags))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			errmsg("Cannot cast temporal float with linear interpolation to temporal integer")));
	/* It is not necessary to set the linear flag to false since it is already
	 * set by the fact that the input argument has step interpolation */
	TSequence *result = tsequence_copy(seq);
	result->valuetypid = INT4OID;
	for (int i = 0; i < seq->count; i++)
	{
		TInstant *inst = tsequence_inst_n(result, i);
		inst->valuetypid = INT4OID;
		Datum *value_ptr = tinstant_value_ptr(inst);
		*value_ptr = Int32GetDatum((double)DatumGetFloat8(tinstant_value(inst)));
	}
	return result;
}

/*****************************************************************************
 * Transformation functions
 *****************************************************************************/

/**
 * Transform the temporal instant value into a temporal sequence value
 */
TSequence *
tinstant_to_tsequence(const TInstant *inst, bool linear)
{
	return tsequence_make((TInstant **)&inst, 1, true, true, linear, NORMALIZE_NO);
}

/**
 * Transform the temporal instant set value into a temporal sequence value
 */
TSequence *
tinstantset_to_tsequence(const TInstantSet *ti, bool linear)
{
	if (ti->count != 1)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			errmsg("Cannot transform input to a temporal sequence")));
	TInstant *inst = tinstantset_inst_n(ti, 0);
	return tsequence_make(&inst, 1, true, true, linear, NORMALIZE_NO);
}

/**
 * Transform the temporal sequence set value into a temporal sequence value
 */
TSequence *
tsequenceset_to_tsequence(const TSequenceSet *ts)
{
	if (ts->count != 1)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			errmsg("Cannot transform input to a temporal sequence")));
	return tsequence_copy(tsequenceset_seq_n(ts, 0));
}

/**
 * Transform the temporal sequence value with continuous base type 
 * from stepwise to linear interpolation
 *
 * @param[out] result Array on which the pointers of the newly constructed 
 * sequences are stored
 * @param[in] seq Temporal value
 * @return Number of resulting sequences returned
 */
int
tstepseq_to_linear1(TSequence **result, const TSequence *seq)
{
	if (seq->count == 1)
	{
		result[0] = tsequence_copy(seq);
		MOBDB_FLAGS_SET_LINEAR(result[0]->flags, true);
		return 1;
	}

	TInstant *inst1 = tsequence_inst_n(seq, 0);
	Datum value1 = tinstant_value(inst1);
	Datum value2;
	TInstant *inst2;
	bool lower_inc = seq->period.lower_inc;
	int k = 0;
	for (int i = 1; i < seq->count; i++)
	{
		inst2 = tsequence_inst_n(seq, i);
		value2 = tinstant_value(inst2);
		TInstant *instants[2];
		instants[0] = inst1;
		instants[1] = tinstant_make(value1, inst2->t, seq->valuetypid);
		bool upper_inc = (i == seq->count - 1) ? seq->period.upper_inc &&
			datum_eq(value1, value2, seq->valuetypid) : false;
		result[k++] = tsequence_make(instants, 2, lower_inc, upper_inc,
			LINEAR, NORMALIZE_NO);
		inst1 = inst2;
		value1 = value2;
		lower_inc = true;
		pfree(instants[1]);
	}
	if (seq->period.upper_inc)
	{
		value1 = tinstant_value(tsequence_inst_n(seq, seq->count - 2));
		value2 = tinstant_value(inst2);
		if (datum_ne(value1, value2, seq->valuetypid))
			result[k++] = tsequence_make(&inst2, 1, true, true,
				LINEAR, NORMALIZE_NO);
	}
	return k;
}

/**
 * Transform the temporal sequence value with continuous base type 
 * from stepwise to linear interpolation
 
 * @param[in] seq Temporal value
 * @return Resulting temporal sequence set value
 */
TSequenceSet *
tstepseq_to_linear(const TSequence *seq)
{
	TSequence **sequences = palloc(sizeof(TSequence *) * seq->count);
	int count = tstepseq_to_linear1(sequences, seq);
	return tsequenceset_make_free(sequences, count, NORMALIZE_NO);
}

/*****************************************************************************
 * Accessor functions
 *****************************************************************************/

/**
 * Returns the distinct base values of the temporal value with stepwise 
 * interpolation
 *
 * @param[in] seq Temporal value
 * @param[out] result Array of Datums
 * @result Number of values in the resulting array
 */
int 
tsequence_values1(Datum *result, const TSequence *seq)
{
	for (int i = 0; i < seq->count; i++)
		result[i] = tinstant_value(tsequence_inst_n(seq, i));
	int count = seq->count;
	if (count > 1)
	{
		datumarr_sort(result, seq->count, seq->valuetypid);
		count = datumarr_remove_duplicates(result, seq->count, seq->valuetypid);
	}
	return count;
}

/**
 * Returns the base values of the temporal value with stepwise 
 * interpolation
 *
 * @param[in] seq Temporal value
 * @result PostgreSQL array of Datums
 */
ArrayType *
tsequence_values(const TSequence *seq)
{
	Datum *values = palloc(sizeof(Datum *) * seq->count);
	int count = tsequence_values1(values, seq);
	ArrayType *result = datumarr_to_array(values, count, seq->valuetypid);
	pfree(values);
	return result;
}

/**
 * Returns the range of base values of the temporal float 
 * with linear interpolation
 *
 * @result C array of ranges
 */
RangeType *
tfloatseq_range(const TSequence *seq)
{
	assert(MOBDB_FLAGS_GET_LINEAR(seq->flags));
	TBOX *box = tsequence_bbox_ptr(seq);
	Datum min = Float8GetDatum(box->xmin);
	Datum max = Float8GetDatum(box->xmax);
	if (box->xmin == box->xmax)
		return range_make(min, max, true, true, FLOAT8OID);

	Datum start = tinstant_value(tsequence_inst_n(seq, 0));
	Datum end = tinstant_value(tsequence_inst_n(seq, seq->count - 1));
	Datum lower, upper;
	bool lower_inc, upper_inc;
	if (DatumGetFloat8(start) < DatumGetFloat8(end))
	{
		lower = start; lower_inc = seq->period.lower_inc;
		upper = end; upper_inc = seq->period.upper_inc;
	}
	else
	{
		lower = end; lower_inc = seq->period.upper_inc;
		upper = start; upper_inc = seq->period.lower_inc;
	}
	bool min_inc = DatumGetFloat8(min) < DatumGetFloat8(lower) ||
		(DatumGetFloat8(min) == DatumGetFloat8(lower) && lower_inc);
	bool max_inc = DatumGetFloat8(max) > DatumGetFloat8(upper) ||
		(DatumGetFloat8(max) == DatumGetFloat8(upper) && upper_inc);
	if (!min_inc || !max_inc)
	{
		for (int i = 1; i < seq->count - 1; i++)
		{
			TInstant *inst = tsequence_inst_n(seq, i);
			if (min_inc || DatumGetFloat8(min) == DatumGetFloat8(tinstant_value(inst)))
				min_inc = true;
			if (max_inc || DatumGetFloat8(max) == DatumGetFloat8(tinstant_value(inst)))
				max_inc = true;
			if (min_inc && max_inc)
				break;
		}
	}
	return range_make(min, max, min_inc, max_inc, FLOAT8OID);
}

/**
 * Returns the ranges of base values of the temporal float 
 * with stepwise interpolation
 *
 * @param[out] result Array on which the pointers of the newly constructed 
 * ranges are stored
 * @param[in] seq Temporal value
 * @result Number of ranges in the result
 */
int
tfloatseq_ranges1(RangeType **result, const TSequence *seq)
{
	/* Temporal float with linear interpolation */
	if (MOBDB_FLAGS_GET_LINEAR(seq->flags))
	{
		result[0] = tfloatseq_range(seq);
		return 1;
	}

	/* Temporal float with step interpolation */
	Datum *values = palloc(sizeof(Datum *) * seq->count);
	int count = tsequence_values1(values, seq);
	for (int i = 0; i < count; i++)
		result[i] = range_make(values[i], values[i], true, true, FLOAT8OID);
	pfree(values);
	return count;
}

/**
 * Returns the ranges of base values of the temporal float 
 * with stepwise interpolation
 *
 * @param[in] seq Temporal value
 * @result PostgreSQL array of ranges
 */
ArrayType *
tfloatseq_ranges(const TSequence *seq)
{
	int count = MOBDB_FLAGS_GET_LINEAR(seq->flags) ? 1 : seq->count;
	RangeType **ranges = palloc(sizeof(RangeType *) * count);
	int count1 = tfloatseq_ranges1(ranges, seq);
	return rangearr_to_array(ranges, count1, type_oid(T_FLOATRANGE), true);
}

/**
 * Returns the time on which the temporal value is defined as a period set
 */
PeriodSet *
tsequence_get_time(const TSequence *seq)
{
	return period_to_periodset_internal(&seq->period);
}

/**
 * Returns a pointer to the instant with minimum base value of the
 * temporal value
 * 
 * The function does not take into account whether the instant is at an
 * exclusive bound or not
 *
 * @note Function used, e.g., for computing the shortest line between two
 * temporal points from their temporal distance
 */
TInstant *
tsequence_min_instant(const TSequence *seq)
{
	Datum min = tinstant_value(tsequence_inst_n(seq, 0));
	int k = 0;
	for (int i = 1; i < seq->count; i++)
	{
		Datum value = tinstant_value(tsequence_inst_n(seq, i));
		if (datum_lt(value, min, seq->valuetypid))
		{
			min = value;
			k = i;
		}
	}
	return tsequence_inst_n(seq, k);
}

/**
 * Returns the minimum base value of the temporal value
 */
Datum
tsequence_min_value(const TSequence *seq)
{
	if (seq->valuetypid == INT4OID)
	{
		TBOX *box = tsequence_bbox_ptr(seq);
		return Int32GetDatum((int)(box->xmin));
	}
	if (seq->valuetypid == FLOAT8OID)
	{
		TBOX *box = tsequence_bbox_ptr(seq);
		return Float8GetDatum(box->xmin);
	}
	Datum result = tinstant_value(tsequence_inst_n(seq, 0));
	for (int i = 1; i < seq->count; i++)
	{
		Datum value = tinstant_value(tsequence_inst_n(seq, i));
		if (datum_lt(value, result, seq->valuetypid))
			result = value;
	}
	return result;
}

/**
 * Returns the maximum base value of the temporal value
 */
Datum
tsequence_max_value(const TSequence *seq)
{
	if (seq->valuetypid == INT4OID)
	{
		TBOX *box = tsequence_bbox_ptr(seq);
		return Int32GetDatum((int)(box->xmax));
	}
	if (seq->valuetypid == FLOAT8OID)
	{
		TBOX *box = tsequence_bbox_ptr(seq);
		return Float8GetDatum(box->xmax);
	}
	Datum result = tinstant_value(tsequence_inst_n(seq, 0));
	for (int i = 1; i < seq->count; i++)
	{
		Datum value = tinstant_value(tsequence_inst_n(seq, i));
		if (datum_gt(value, result, seq->valuetypid))
			result = value;
	}
	return result;
}

/**
 * Returns the timespan of the temporal value
 */
Datum
tsequence_timespan(const TSequence *seq)
{
	Interval *result = period_timespan_internal(&seq->period);
	return PointerGetDatum(result);
}

/**
 * Returns the bounding period on which the temporal value is defined
 */
void
tsequence_period(Period *p, const TSequence *seq)
{
	period_set(p, seq->period.lower, seq->period.upper,
		seq->period.lower_inc, seq->period.upper_inc);
}

/**
 * Returns the distinct instants of the temporal value as a C array
 */
TInstant **
tsequence_instants(const TSequence *seq)
{
	TInstant **result = palloc(sizeof(TInstant *) * seq->count);
	for (int i = 0; i < seq->count; i++)
		result[i] = tsequence_inst_n(seq, i);
	return result;
}

/**
 * Returns the distinct instants of the temporal value as a PostgreSQL array
 */
ArrayType *
tsequence_instants_array(const TSequence *seq)
{
	TInstant **instants = tsequence_instants(seq);
	ArrayType *result = temporalarr_to_array((Temporal **)instants, seq->count);
	pfree(instants);
	return result;
}

/**
 * Returns the start timestamp of the temporal value
 */
TimestampTz
tsequence_start_timestamp(const TSequence *seq)
{
	return (tsequence_inst_n(seq, 0))->t;
}

/**
 * Returns the end timestamp of the temporal value
 */
TimestampTz
tsequence_end_timestamp(const TSequence *seq)
{
	return (tsequence_inst_n(seq, seq->count - 1))->t;
}

/**
 * Returns the timestamps of the temporal value as a C array
 */
int
tsequence_timestamps1(TimestampTz *times, const TSequence *seq)
{
	for (int i = 0; i < seq->count; i++)
		times[i] = tsequence_inst_n(seq, i)->t;
	return seq->count;
}

/**
 * Returns the timestamps of the temporal value as a PostgreSQL array
 */
ArrayType *
tsequence_timestamps(const TSequence *seq)
{
	TimestampTz *times = palloc(sizeof(TimestampTz) * seq->count);
	tsequence_timestamps1(times, seq);
	ArrayType *result = timestamparr_to_array(times, seq->count);
	pfree(times);
	return result;
}

/**
 * Shift the time span of the temporal value by the interval
 */
TSequence *
tsequence_shift(const TSequence *seq, const Interval *interval)
{
	TSequence *result = tsequence_copy(seq);
	TInstant **instants = palloc(sizeof(TInstant *) * seq->count);
	for (int i = 0; i < seq->count; i++)
	{
		TInstant *inst = instants[i] = tsequence_inst_n(result, i);
		inst->t = DatumGetTimestampTz(
			DirectFunctionCall2(timestamptz_pl_interval,
			TimestampTzGetDatum(inst->t), PointerGetDatum(interval)));
	}
	/* Shift period */
	result->period.lower = DatumGetTimestampTz(
			DirectFunctionCall2(timestamptz_pl_interval,
			TimestampTzGetDatum(seq->period.lower), PointerGetDatum(interval)));
	result->period.upper = DatumGetTimestampTz(
			DirectFunctionCall2(timestamptz_pl_interval,
			TimestampTzGetDatum(seq->period.upper), PointerGetDatum(interval)));
	/* Shift bounding box */
	void *bbox = tsequence_bbox_ptr(result);
	temporal_bbox_shift(bbox, interval, seq->valuetypid);
	pfree(instants);
	return result;
}

/*****************************************************************************
 * Ever/always comparison operators
 * The functions assume that the temporal value and the datum value are of
 * the same valuetypid. Ever/always equal are valid for all temporal types
 * including temporal points. All the other comparisons are only valid for
 * temporal alphanumeric types.
 *****************************************************************************/

/**
 * Returns true if the segment of the temporal sequence value with
 * linear interpolation is ever equal to the base value
 *
 * @param[in] inst1,inst2 Instants defining the segment
 * @param[in] lower_inc,upper_inc Upper and lower bounds of the segment
 * @param[in] value Base value
 */
static bool
tlinearseq_ever_eq1(const TInstant *inst1, const TInstant *inst2,
	bool lower_inc, bool upper_inc, Datum value)
{
	Datum value1 = tinstant_value(inst1);
	Datum value2 = tinstant_value(inst2);
	Oid valuetypid = inst1->valuetypid;

	/* Constant segment */
	if (datum_eq(value1, value2, valuetypid) &&
		datum_eq(value1, value, valuetypid))
		return true;

	/* Test of bounds */
	if (datum_eq(value1, value, valuetypid))
		return lower_inc;
	if (datum_eq(value2, value, valuetypid))
		return upper_inc;

	/* Interpolation for continuous base type */
	return tlinearseq_intersection_value(inst1, inst2, value, valuetypid,
		NULL, NULL);
}

/**
 * Returns true if the temporal value is ever equal to the base value
 */
bool
tsequence_ever_eq(const TSequence *seq, Datum value)
{
	/* Bounding box test */
	if (! temporal_bbox_ever_eq((Temporal *)seq, value))
		return false;

	if (! MOBDB_FLAGS_GET_LINEAR(seq->flags) || seq->count == 1)
	{
		/* Stepwise interpolation*/
		for (int i = 0; i < seq->count; i++)
		{
			Datum valueinst = tinstant_value(tsequence_inst_n(seq, i));
			if (datum_eq(valueinst, value, seq->valuetypid))
				return true;
		}
		return false;
	}

	/* Linear interpolation*/
	TInstant *inst1 = tsequence_inst_n(seq, 0);
	bool lower_inc = seq->period.lower_inc;
	for (int i = 1; i < seq->count; i++)
	{
		TInstant *inst2 = tsequence_inst_n(seq, i);
		bool upper_inc = (i == seq->count - 1) ? seq->period.upper_inc : false;
		if (tlinearseq_ever_eq1(inst1, inst2, lower_inc, upper_inc, value))
			return true;
		inst1 = inst2;
		lower_inc = true;
	}
	return false;
}

/**
 * Returns true if the temporal value is always equal to the base value
 */
bool
tsequence_always_eq(const TSequence *seq, Datum value)
{
	/* Bounding box test */
	if (! temporal_bbox_always_eq((Temporal *)seq, value))
		return false;

	/* The bounding box test above is enough to compute
	 * the answer for temporal numbers and points */
	if (numeric_base_type(seq->valuetypid) ||
		point_base_type(seq->valuetypid))
		return true;

	/* The following test assumes that the sequence is in normal form */
	if (seq->count > 2)
		return false;
	for (int i = 0; i < seq->count; i++)
	{
		Datum valueinst = tinstant_value(tsequence_inst_n(seq, i));
		if (datum_ne(valueinst, value, seq->valuetypid))
			return false;
	}
	return true;
}

/*****************************************************************************/

/**
 * Returns true if the segment of the temporal value with linear 
 * interpolation is ever less than or equal to the base value
 *
 * @param[in] value1,value2 Input base values
 * @param[in] valuetypid Oid of the base type
 * @param[in] lower_inc,upper_inc Upper and lower bounds of the segment
 * @param[in] value Base value
 */
static bool
tlinearseq_ever_le1(Datum value1, Datum value2, Oid valuetypid,
	bool lower_inc, bool upper_inc, Datum value)
{
	/* Constant segment */
	if (datum_eq(value1, value2, valuetypid))
		return datum_le(value1, value, valuetypid);
	/* Increasing segment */
	if (datum_lt(value1, value2, valuetypid))
		return datum_lt(value1, value, valuetypid) ||
			(lower_inc && datum_eq(value1, value, valuetypid));
	/* Decreasing segment */
	return datum_lt(value2, value, valuetypid) ||
		(upper_inc && datum_eq(value2, value, valuetypid));
}

/**
 * Returns true if the segment of the temporal value with linear 
 * interpolation is always less than the base value
 *
 * @param[in] value1,value2 Input base values
 * @param[in] valuetypid Oid of the base type
 * @param[in] lower_inc,upper_inc Upper and lower bounds of the segment
 * @param[in] value Base value
 */
static bool
tlinearseq_always_lt1(Datum value1, Datum value2, Oid valuetypid,
	bool lower_inc, bool upper_inc, Datum value)
{
	/* Constant segment */
	if (datum_eq(value1, value2, valuetypid))
		return datum_lt(value1, value1, valuetypid);
	/* Increasing segment */
	if (datum_lt(value1, value2, valuetypid))
		return datum_lt(value2, value, valuetypid) ||
			(! upper_inc && datum_eq(value, value2, valuetypid));
	/* Decreasing segment */
	return datum_lt(value1, value, valuetypid) ||
		(! lower_inc && datum_eq(value1, value, valuetypid));
}

/*****************************************************************************/

/**
 * Returns true if the temporal value is ever less than the base value
 */
bool
tsequence_ever_lt(const TSequence *seq, Datum value)
{
	/* Bounding box test */
	if (! temporal_bbox_ever_lt_le((Temporal *)seq, value))
		return false;

	for (int i = 0; i < seq->count; i++)
	{
		Datum valueinst = tinstant_value(tsequence_inst_n(seq, i));
		if (datum_lt(valueinst, value, seq->valuetypid))
			return true;
	}
	return false;
}

/**
 * Returns true if the temporal value is ever less than or equal to
 * the base value
 */
bool
tsequence_ever_le(const TSequence *seq, Datum value)
{
	/* Bounding box test */
	if (! temporal_bbox_ever_lt_le((Temporal *)seq, value))
		return false;

	if (! MOBDB_FLAGS_GET_LINEAR(seq->flags) || seq->count == 1)
	{
		/* Stepwise interpolation */
		for (int i = 0; i < seq->count; i++)
		{
			Datum valueinst = tinstant_value(tsequence_inst_n(seq, i));
			if (datum_le(valueinst, value, seq->valuetypid))
				return true;
		}
		return false;
	}

	/* Linear interpolation */
	Datum value1 = tinstant_value(tsequence_inst_n(seq, 0));
	bool lower_inc = seq->period.lower_inc;
	for (int i = 1; i < seq->count; i++)
	{
		Datum value2 = tinstant_value(tsequence_inst_n(seq, i));
		bool upper_inc = (i == seq->count - 1) ? seq->period.upper_inc : false;
		if (tlinearseq_ever_le1(value1, value2, seq->valuetypid,
			lower_inc, upper_inc, value))
			return true;
		value1 = value2;
		lower_inc = true;
	}
	return false;
}

/**
 * Returns true if the temporal value is always less than the base value
 */
bool
tsequence_always_lt(const TSequence *seq, Datum value)
{
	/* Bounding box test */
	if (! temporal_bbox_always_lt_le((Temporal *)seq, value))
		return false;

	if (! MOBDB_FLAGS_GET_LINEAR(seq->flags) || seq->count == 1)
	{
		/* Stepwise interpolation */
		for (int i = 0; i < seq->count; i++)
		{
			Datum valueinst = tinstant_value(tsequence_inst_n(seq, i));
			if (! datum_lt(valueinst, value, seq->valuetypid))
				return false;
		}
		return true;
	}

	/* Linear interpolation */
	Datum value1 = tinstant_value(tsequence_inst_n(seq, 0));
	bool lower_inc = seq->period.lower_inc;
	for (int i = 1; i < seq->count; i++)
	{
		Datum value2 = tinstant_value(tsequence_inst_n(seq, i));
		bool upper_inc = (i == seq->count - 1) ? seq->period.upper_inc : false;
		if (! tlinearseq_always_lt1(value1, value2, seq->valuetypid,
			lower_inc, upper_inc, value))
			return false;
		value1 = value2;
		lower_inc = true;
	}
	return true;
}

/**
 * Returns true if the temporal value is always less than or equal to
 * the base value
 */
bool
tsequence_always_le(const TSequence *seq, Datum value)
{
	/* Bounding box test */
	if (! temporal_bbox_always_lt_le((Temporal *)seq, value))
		return false;

	/* The bounding box test above is enough to compute
	 * the answer for temporal numbers */
	if (numeric_base_type(seq->valuetypid))
		return true;

	/* We are sure that the type has stewpwise interpolation since
	 * there are currenty no other continuous base type besides tfloat
	 * to which the always <= comparison applies */
	assert(! MOBDB_FLAGS_GET_LINEAR(seq->flags));
	for (int i = 0; i < seq->count; i++)
	{
		Datum valueinst = tinstant_value(tsequence_inst_n(seq, i));
		if (! datum_le(valueinst, value, seq->valuetypid))
			return false;
	}
	return true;
}

/*****************************************************************************
 * Restriction Functions
 *****************************************************************************/

/**
 * Restricts the segment of a temporal value to the base value
 *
 * @param[in] inst1,inst2 Temporal values defining the segment 
 * @param[in] linear True when the segment has linear interpolation
 * @param[in] lower_inc,upper_inc Upper and lower bounds of the segment
 * @param[in] value Base value
 * @return Resulting temporal sequence
 */
static TSequence *
tsequence_at_value1(const TInstant *inst1, const TInstant *inst2,
	bool linear, bool lower_inc, bool upper_inc, Datum value)
{
	Datum value1 = tinstant_value(inst1);
	Datum value2 = tinstant_value(inst2);
	Oid valuetypid = inst1->valuetypid;
	TInstant *instants[2];

	/* Constant segment (step or linear interpolation) */
	if (datum_eq(value1, value2, valuetypid))
	{
		/* If not equal to value */
		if (datum_ne(value1, value, valuetypid))
			return NULL;
		instants[0] = (TInstant *) inst1;
		instants[1] = (TInstant *) inst2;
		TSequence *result = tsequence_make(instants, 2, lower_inc,
			upper_inc, linear, NORMALIZE_NO);
		return result;
	}

	/* Stepwise interpolation */
	if (! linear)
	{
		TSequence *result = NULL;
		if (datum_eq(value1, value, valuetypid))
		{
			/* <value@t1 x@t2> */
			instants[0] = (TInstant *) inst1;
			instants[1] = tinstant_make(value1, inst2->t, valuetypid);
			result = tsequence_make(instants, 2, lower_inc, false,
				linear, NORMALIZE_NO);
			pfree(instants[1]);
		}
		else if (upper_inc && datum_eq(value, value2, valuetypid))
		{
			/* <x@t1 value@t2] */
			result = tsequence_make((TInstant **)&inst2, 1, true, true,
				linear, NORMALIZE_NO);
		}
		return result;
	}

	/* Linear interpolation: Test of bounds */
	if (datum_eq(value1, value, valuetypid))
	{
		if (!lower_inc)
			return NULL;
		return tsequence_make((TInstant **)&inst1, 1, true, true,
			linear, NORMALIZE_NO);
	}
	if (datum_eq(value2, value, valuetypid))
	{
		if (!upper_inc)
			return NULL;
		return tsequence_make((TInstant **)&inst2, 1, true, true,
			linear, NORMALIZE_NO);
	}

	/* Interpolation */
	Datum projvalue;
	TimestampTz t;
	if (! tlinearseq_intersection_value(inst1, inst2, value, valuetypid,
		&projvalue, &t))
		return NULL;

	TInstant *inst = tinstant_make(projvalue, t, valuetypid);
	TSequence *result = tsequence_make(&inst, 1, true, true, linear,
		NORMALIZE_NO);
	pfree(inst); DATUM_FREE(projvalue, valuetypid);
	return result;
}

/**
 * Restricts the temporal value to the base value
 *
 * @param[out] result Array on which the pointers of the newly constructed 
 * sequences are stored
 * @param[in] seq Temporal value
 * @param[in] value Base value
 * @return Number of resulting sequences returned
 * @note This function is called for each sequence of a temporal sequence set.
 * For this reason the bounding box and the instantaneous sequence sets are 
 * repeated here.
 */
int
tsequence_at_value(TSequence **result, const TSequence *seq, Datum value)
{
	/* Instantaneous sequence */
	if (seq->count == 1)
	{
		TInstant *inst = tsequence_inst_n(seq, 0);
		if (datum_ne(tinstant_value(inst), value, seq->valuetypid))
			return 0;
		result[0] = tsequence_copy(seq);
		return 1;
	}

	/* Bounding box test */
	if (! temporal_bbox_restrict_value((Temporal *)seq, value))
		return 0;

	/* General case */
	TInstant *inst1 = tsequence_inst_n(seq, 0);
	bool linear = MOBDB_FLAGS_GET_LINEAR(seq->flags);
	bool lower_inc = seq->period.lower_inc;
	int k = 0;
	for (int i = 1; i < seq->count; i++)
	{
		TInstant *inst2 = tsequence_inst_n(seq, i);
		bool upper_inc = (i == seq->count - 1) ? seq->period.upper_inc : false;
		TSequence *seq1 = tsequence_at_value1(inst1, inst2, linear,
			lower_inc, upper_inc, value);
		if (seq1 != NULL)
			result[k++] = seq1;
		inst1 = inst2;
		lower_inc = true;
	}
	return k;
}

/**
 * Restricts the segment of a temporal value with linear interpolation
 * to the complement of the base value
 *
 * @param[out] result Array on which the pointers of the newly constructed 
 * sequences are stored
 * @param[in] inst1,inst2 Temporal values defining the segment 
 * @param[in] lower_inc,upper_inc Upper and lower bounds of the segment
 * @param[in] value Base value
 * @return Number of resulting sequences returned
 */
static int
tlinearseq_minus_value1(TSequence **result,
	const TInstant *inst1, const TInstant *inst2,
	bool lower_inc, bool upper_inc, Datum value)
{
	Datum value1 = tinstant_value(inst1);
	Datum value2 = tinstant_value(inst2);
	Oid valuetypid = inst1->valuetypid;
	TInstant *instants[2];

	/* Constant segment */
	if (datum_eq(value1, value2, valuetypid))
	{
		/* Equal to value */
		if (datum_eq(value1, value, valuetypid))
			return 0;

		instants[0] = (TInstant *) inst1;
		instants[1] = (TInstant *) inst2;
		result[0] = tsequence_make(instants, 2, lower_inc, upper_inc, 
			LINEAR, NORMALIZE_NO);
		return 1;
	}

	/* Test of bounds */
	if (datum_eq(value1, value, valuetypid))
	{
		instants[0] = (TInstant *) inst1;
		instants[1] = (TInstant *) inst2;
		result[0] = tsequence_make(instants, 2, false, upper_inc,
			LINEAR, NORMALIZE_NO);
		return 1;
	}
	if (datum_eq(value2, value, valuetypid))
	{
		instants[0] = (TInstant *) inst1;
		instants[1] = (TInstant *) inst2;
		result[0] = tsequence_make(instants, 2, lower_inc, false, 
			LINEAR, NORMALIZE_NO);
		return 1;
	}

	/* Linear interpolation */
	Datum projvalue;
	TimestampTz t;
	if (!tlinearseq_intersection_value(inst1, inst2, value, valuetypid,
		&projvalue, &t))
	{
		instants[0] = (TInstant *) inst1;
		instants[1] = (TInstant *) inst2;
		result[0] = tsequence_make(instants, 2, lower_inc, upper_inc,
			LINEAR, NORMALIZE_NO);
		return 1;
	}
	instants[0] = (TInstant *) inst1;
	instants[1] = tinstant_make(projvalue, t, valuetypid);
	result[0] = tsequence_make(instants, 2, lower_inc, false,
		LINEAR, NORMALIZE_NO);
	instants[0] = instants[1];
	instants[1] = (TInstant *) inst2;
	result[1] = tsequence_make(instants, 2, false, upper_inc,
		LINEAR, NORMALIZE_NO);
	pfree(instants[0]); DATUM_FREE(projvalue, valuetypid);
	return 2;
}

/**
 * Restricts the temporal value to the complement of the base value
 *
 * @param[out] result Array on which the pointers of the newly constructed 
 * sequences are stored
 * @param[in] seq Temporal value
 * @param[in] value Base value
 * @return Number of resulting sequences returned
 * @note This function is called for each sequence of a temporal sequence set
 */
int
tsequence_minus_value(TSequence **result, const TSequence *seq, Datum value)
{
	/* Instantaneous sequence */
	if (seq->count == 1)
	{
		TInstant *inst = tsequence_inst_n(seq, 0);
		if (datum_eq(tinstant_value(inst), value, seq->valuetypid))
			return 0;
		result[0] = tsequence_copy(seq);
		return 1;
	}

	/* Bounding box test */
	if (! temporal_bbox_restrict_value((Temporal *)seq, value))
	{
		result[0] = tsequence_copy(seq);
		return 1;
	}

	/* General case */
	int k = 0;
	if (! MOBDB_FLAGS_GET_LINEAR(seq->flags))
	{
		/* Stepwise interpolation */
		TInstant **instants = palloc(sizeof(TInstant *) * seq->count);
		bool lower_inc = seq->period.lower_inc;
		int j = 0;
		for (int i = 0; i < seq->count; i++)
		{
			TInstant *inst = tsequence_inst_n(seq, i);
			Datum value1 = tinstant_value(inst);
			if (datum_eq(value1, value, seq->valuetypid))
			{
				if (j > 0)
				{
					instants[j] = tinstant_make(tinstant_value(instants[j - 1]),
						inst->t, seq->valuetypid);
					result[k++] = tsequence_make(instants, j + 1, lower_inc,
						false, STEP, NORMALIZE_NO);
					pfree(instants[j]);
					j = 0;
				}
				lower_inc = true;
			}
			else
				instants[j++] = inst;
		}
		if (j > 0)
			result[k++] = tsequence_make(instants, j, lower_inc,
				seq->period.upper_inc, STEP, NORMALIZE_NO);
		pfree(instants);
	}
	else
	{
		/* Linear interpolation */
		bool lower_inc = seq->period.lower_inc;
		TInstant *inst1 = tsequence_inst_n(seq, 0);
		for (int i = 1; i < seq->count; i++)
		{
			TInstant *inst2 = tsequence_inst_n(seq, i);
			bool upper_inc = (i == seq->count - 1) ? seq->period.upper_inc : false;
			/* The next step adds between one and two sequences */
			k += tlinearseq_minus_value1(&result[k], inst1, inst2,
				lower_inc, upper_inc, value);
			inst1 = inst2;
			lower_inc = true;
		}
	}	
	return k;
}

/**
 * Restricts the temporal value to (the complement of) the base value
 *
 * @param[in] seq Temporal value
 * @param[in] value Base values
 * @param[in] atfunc True when the restriction is at, false for minus 
 * @note There is no bounding box or instantaneous test in this function, 
 * they are done in the atValue and minusValue functions since the latter are
 * called for each sequence in a sequence set or for each element in the array
 * for the atValues and minusValues functions.
 */
TSequenceSet *
tsequence_restrict_value(const TSequence *seq, Datum value, bool atfunc)
{
	int count = seq->count;
	/* For minus and linear interpolation we need the double of the count */
	if (!atfunc && MOBDB_FLAGS_GET_LINEAR(seq->flags))
		count *= 2;
	TSequence **sequences = palloc(sizeof(TSequence *) * count);
	int newcount = atfunc ? tsequence_at_value(sequences, seq, value) :
		tsequence_minus_value(sequences, seq, value);
	return tsequenceset_make_free(sequences, newcount, NORMALIZE);
}

/**
 * Restricts the temporal value to the array of base values
 *
 * @param[out] result Array on which the pointers of the newly constructed 
 * sequences are stored
 * @param[in] seq Temporal value
 * @param[in] values Array of base values
 * @param[in] count Number of elements in the input array
 * @param[in] atfunc True when the restriction is at, false for minus 
 * @return Number of resulting sequences returned
 * @pre There are no duplicates values in the array
 * @note This function is called for each sequence of a temporal sequence set
 */
int
tsequence_at_values1(TSequence **result, const TSequence *seq,
	const Datum *values, int count)
{
	/* Instantaneous sequence */
	if (seq->count == 1)
	{
		TInstant *inst = tsequence_inst_n(seq, 0);
		TInstant *inst1 = tinstant_restrict_values(inst, values, count, REST_AT);
		if (inst1 == NULL)
			return 0;
		pfree(inst1); 
		result[0] = tsequence_copy(seq);
		return 1;
	}

	/* Bounding box test */
	int count1;
	Datum *values1 = temporal_bbox_restrict_values((Temporal *)seq, values,
		count, &count1);
	if (count1 == 0)
		return 0;

	/* General case */
	TInstant *inst1 = tsequence_inst_n(seq, 0);
	bool lower_inc = seq->period.lower_inc;
	bool linear = MOBDB_FLAGS_GET_LINEAR(seq->flags);
	int k = 0;
	for (int i = 1; i < seq->count; i++)
	{
		TInstant *inst2 = tsequence_inst_n(seq, i);
		bool upper_inc = (i == seq->count - 1) ? seq->period.upper_inc : false;
		for (int j = 0; j < count; j++)
		{
			TSequence *seq1 = tsequence_at_value1(inst1, inst2, 
				linear, lower_inc, upper_inc, values[j]);
			if (seq1 != NULL) 
				result[k++] = seq1;
		}
		inst1 = inst2;
		lower_inc = true;
	}
	if (k > 1)
		tsequencearr_sort(result, k);

	pfree(values1);
	return k;
}

/**
 * Restricts the temporal value to (the complement of) the array of base values
 *
 * @param[in] seq Temporal value
 * @param[in] values Array of base values
 * @param[in] count Number of elements in the input array
 * @param[in] atfunc True when the restriction is at, false for minus 
 * @return Resulting temporal sequence set value
 */
TSequenceSet *
tsequence_restrict_values(const TSequence *seq, const Datum *values, int count,
	bool atfunc)
{
	/* Bounding box test */
	int count1;
	Datum *values1 = temporal_bbox_restrict_values((Temporal *)seq, values, count, 
		&count1);
	if (count1 == 0)
	{
		if (atfunc)
			return NULL;
		else
			return tsequence_to_tsequenceset(seq);
	}
	
	/* General case */
	TSequence **sequences = palloc(sizeof(TSequence *) * seq->count * count1 * 2);
	int newcount = tsequence_at_values1(sequences, seq, values1, count1);
	pfree(values1);
	TSequenceSet *atresult = tsequenceset_make_free(sequences, newcount, NORMALIZE);
	if (atfunc)
		return atresult;
	
	/* 
	 * MINUS function
	 * Compute the complement of the previous value.
	 */
	if (newcount == 0)
		return tsequence_to_tsequenceset(seq);

	PeriodSet *ps1 = tsequenceset_get_time(atresult);
	PeriodSet *ps2 = minus_period_periodset_internal(&seq->period, ps1);
	TSequenceSet *result = NULL;
	if (ps2 != NULL)
	{
		result = tsequence_restrict_periodset(seq, ps2, REST_AT);
		pfree(ps2);
	}
	pfree(atresult); pfree(ps1); 
	return result;
}

/**
 * Restricts the segment of a temporal number to the range of
 * base values
 *
 * @param[in] inst1,inst2 Temporal values defining the segment 
 * @param[in] lower_incl,upper_incl Upper and lower bounds of the segment
 * @param[in] linear True when the segment has linear interpolation
 * @param[in] range Range of base values
 * @return Resulting temporal sequence value
 */
static TSequence *
tnumberseq_at_range1(const TInstant *inst1, const TInstant *inst2,
	bool lower_incl, bool upper_incl, bool linear, RangeType *range)
{
	TypeCacheEntry *typcache = lookup_type_cache(range->rangetypid,
		TYPECACHE_RANGE_INFO);
	Datum value1 = tinstant_value(inst1);
	Datum value2 = tinstant_value(inst2);
	Oid valuetypid = inst1->valuetypid;
	TInstant *instants[2];
	/* Stepwise interpolation or constant segment */
	if (! linear || datum_eq(value1, value2, valuetypid))
	{
		if (! range_contains_elem_internal(typcache, range, value1))
			return NULL;

		instants[0] = (TInstant *) inst1;
		instants[1] = linear ? (TInstant *) inst2 :
			tinstant_make(value1, inst2->t, valuetypid);
		/* Stepwise segment with inclusive upper bound must exclude that bound */
		bool upper_incl1 = (linear) ? upper_incl : false;
		TSequence *result = tsequence_make(instants, 2, lower_incl,
			upper_incl1, linear, NORMALIZE_NO);
		return result;
	}

	/* Ensure data type with linear interpolation */
	assert(valuetypid == FLOAT8OID);
	bool increasing = DatumGetFloat8(value1) < DatumGetFloat8(value2);
	RangeType *valuerange = increasing ?
		range_make(value1, value2, lower_incl, upper_incl, FLOAT8OID) :
		range_make(value2, value1, upper_incl, lower_incl, FLOAT8OID);
#if MOBDB_PGSQL_VERSION < 110000
	RangeType *intersect = DatumGetRangeType(call_function2(range_intersect, 
		PointerGetDatum(valuerange), PointerGetDatum(range)));
#else
	RangeType *intersect = DatumGetRangeTypeP(call_function2(range_intersect, 
		PointerGetDatum(valuerange), PointerGetDatum(range)));
#endif
	pfree(valuerange);
	if (RangeIsEmpty(intersect))
	{
		pfree(intersect);
		return NULL;
	}

	/* We are sure that neither lower or upper are infinite */
	Datum lower = lower_datum(intersect);
	Datum upper = upper_datum(intersect);
	bool lower_inc2 = lower_inc(intersect);
	bool upper_inc2 = upper_inc(intersect);
	pfree(intersect);
	double dlower = DatumGetFloat8(lower);
	double dupper = DatumGetFloat8(upper);
	double dvalue1 = DatumGetFloat8(value1);
	double dvalue2 = DatumGetFloat8(value2);
	TSequence *result;
	TimestampTz t1, t2;
	bool foundlower = false, foundupper = false;
	if (dlower == dupper)
	{
		t1 = dlower == dvalue1 ? inst1->t : inst2->t;
		instants[0] = tinstant_make(lower, t1, valuetypid);
		result = tsequence_make(instants, 1, true, true, linear, NORMALIZE_NO);
		pfree(instants[0]);
		return result;
	}

	double min = Min(dvalue1, dvalue2);
	double max = Max(dvalue1, dvalue2);
	if (min <= dlower && dlower <= max)
		foundlower = tnumberseq_intersection_value(inst1, inst2, lower,
			FLOAT8OID, &t1);
	if (dlower != dupper && min <= dupper && dupper <= max)
		foundupper = tnumberseq_intersection_value(inst1, inst2, upper,
			FLOAT8OID, &t2);

	if (! foundlower && !foundupper)
	{
		instants[0] = (TInstant *) inst1;
		instants[1] = (TInstant *) inst2;
		return tsequence_make(instants, 2, lower_incl, upper_incl,
			linear, NORMALIZE_NO);
	}
	if (foundlower && foundupper)
	{
		instants[0] = tsequence_at_timestamp1(inst1, inst2, linear, Min(t1, t2));
		instants[1] = tsequence_at_timestamp1(inst1, inst2, linear, Max(t1, t2));
		result = tsequence_make(instants, 2, lower_inc2, upper_inc2,
			linear, NORMALIZE_NO);
		pfree(instants[0]); pfree(instants[1]);
		return result;
	}
	if (foundlower)
	{
		if (increasing)
		{
			instants[0] = tsequence_at_timestamp1(inst1, inst2, linear, t1);
			instants[1] = (TInstant *) inst2;
			result = tsequence_make(instants, 2, lower_inc2, upper_incl,
				linear, NORMALIZE_NO);
			pfree(instants[0]);
		}
		else
		{
			instants[0] = (TInstant *) inst1;
			instants[1] = tsequence_at_timestamp1(inst1, inst2, linear, t1);
			result = tsequence_make(instants, 2, lower_incl, upper_inc2,
				linear, NORMALIZE_NO);
			pfree(instants[1]);
		}
		return result;
	}
	else /* foundupper */
	{
		if (increasing)
		{
			instants[0] = (TInstant *) inst1;
			instants[1] = tsequence_at_timestamp1(inst1, inst2, linear, t2);
			result = tsequence_make(instants, 2, lower_incl, upper_inc2,
				linear, NORMALIZE_NO);
			pfree(instants[1]);
		}
		else
		{
			instants[0] = tsequence_at_timestamp1(inst1, inst2, linear, t2);
			instants[1] = (TInstant *) inst2;
			result = tsequence_make(instants, 2, lower_inc2, upper_incl,
				linear, NORMALIZE_NO);
			pfree(instants[0]);
		}
		return result;
	}
}

/**
 * Restricts the temporal number to the (complement of the) range of
 * base values
 *
 * @param[out] result Array on which the pointers of the newly constructed 
 * sequences are stored
 * @param[in] seq temporal number
 * @param[in] range Range of base values
 * @param[in] atfunc True when the restriction is at, false for minus 
 * @return Number of resulting sequences returned
 * @note This function is called for each sequence of a temporal sequence set 
 */
int 
tnumberseq_restrict_range1(TSequence **result, const TSequence *seq, 
	RangeType *range, bool atfunc)
{
	/* Bounding box test */
	TBOX box1, box2;
	memset(&box1, 0, sizeof(TBOX));
	memset(&box2, 0, sizeof(TBOX));
	tsequence_bbox(&box1, seq);
	range_to_tbox_internal(&box2, range);
	if (!overlaps_tbox_tbox_internal(&box1, &box2))
	{
		if (atfunc) 
			return 0;
		else
		{
			result[0] = tsequence_copy(seq);
			return 1;
		}
	}

	/* Instantaneous sequence */
	if (seq->count == 1)
	{
		/* The bounding box test above does not distinguish between 
		 * inclusive/exclusive bounds */
		TInstant *inst = tsequence_inst_n(seq, 0);
		TInstant *inst1 = tnumberinst_restrict_range(inst, range, atfunc);
		if (inst1 == NULL)
			return 0;
		pfree(inst1); 
		result[0] = tsequence_copy(seq);
		return 1;
	}

	/* General case */
	if (atfunc)
	{
		/* AT function */
		TInstant *inst1 = tsequence_inst_n(seq, 0);
		bool lower_inc = seq->period.lower_inc;
		bool linear = MOBDB_FLAGS_GET_LINEAR(seq->flags);
		int k = 0;
		for (int i = 1; i < seq->count; i++)
		{
			TInstant *inst2 = tsequence_inst_n(seq, i);
			bool upper_inc = (i == seq->count - 1) ? seq->period.upper_inc : false;
			TSequence *seq1 = tnumberseq_at_range1(inst1, inst2, 
				lower_inc, upper_inc, linear, range);
			if (seq1 != NULL) 
				result[k++] = seq1;
			inst1 = inst2;
			lower_inc = true;
		}
		/* Stepwise sequence with inclusive upper bound must add a sequence for that bound */
		if (! linear && seq->period.upper_inc)
		{
			inst1 = tsequence_inst_n(seq, seq->count - 1);
			Datum value = tinstant_value(inst1);
			TypeCacheEntry *typcache = lookup_type_cache(range->rangetypid,
				TYPECACHE_RANGE_INFO);
			if (range_contains_elem_internal(typcache, range, value))
				result[k++] = tsequence_make(&inst1, 1, true, true, STEP, NORMALIZE_NO);
		}
		return k;
	}
	else
	{
		/* MINUS function
		 * Compute first tnumberseq_at_range, then compute its complement */
		TSequenceSet *ts = tnumberseq_restrict_range(seq, range, REST_AT);
		if (ts == NULL)
		{
			result[0] = tsequence_copy(seq);
			return 1;
		}
		PeriodSet *ps1 = tsequenceset_get_time(ts);
		PeriodSet *ps2 = minus_period_periodset_internal(&seq->period, ps1);
		int count = 0;
		if (ps2 != NULL)
		{
			count = tsequence_at_periodset(result, seq, ps2);
			pfree(ps2);
		}
		pfree(ts); pfree(ps1); 
		return count;
	}
}

/**
 * Restricts the temporal number to the (complement of the) range of base values
 *
 * @param[in] seq temporal number
 * @param[in] range Range of base values
 * @param[in] atfunc True when the restriction is at, false for minus 
 * @return Resulting temporal sequence set value
 */
TSequenceSet *
tnumberseq_restrict_range(const TSequence *seq, RangeType *range, bool atfunc)
{
	int count = seq->count;
	/* For minus and linear interpolation we need the double of the count */
	if (!atfunc && MOBDB_FLAGS_GET_LINEAR(seq->flags))
		count *= 2;
	TSequence **sequences = palloc(sizeof(TSequence *) * count);
	int newcount = tnumberseq_restrict_range1(sequences, seq, range, atfunc);
	return tsequenceset_make_free(sequences, newcount, NORMALIZE);
}

/**
 * Restricts the temporal number to the (complement of the) array of ranges
 * of base values
 *
 * @param[out] result Array on which the pointers of the newly constructed 
 * sequences are stored
 * @param[in] seq temporal number
 * @param[in] normranges Array of ranges of base values
 * @param[in] count Number of elements in the input array
 * @param[in] atfunc True when the restriction is at, false for minus 
 * @return Number of resulting sequences returned
 * @pre The array of ranges is normalized
 * @note This function is called for each sequence of a temporal sequence set 
 */
int
tnumberseq_restrict_ranges1(TSequence **result, const TSequence *seq,
	RangeType **normranges, int count, bool atfunc)
{
	/* Instantaneous sequence */
	if (seq->count == 1)
	{
		TInstant *inst = tsequence_inst_n(seq, 0);
		TInstant *inst1 = tnumberinst_restrict_ranges(inst, normranges, count,
			atfunc);
		if (inst1 == NULL)
			return 0;
		pfree(inst1); 
		result[0] = tsequence_copy(seq);
		return 1;
	}

	/* General case */
	if (atfunc)
	{
		/* AT function */
		TInstant *inst1 = tsequence_inst_n(seq, 0);
		bool lower_inc = seq->period.lower_inc;
		bool linear = MOBDB_FLAGS_GET_LINEAR(seq->flags);
		int k = 0;
		for (int i = 1; i < seq->count; i++)
		{
			TInstant *inst2 = tsequence_inst_n(seq, i);
			bool upper_inc = (i == seq->count - 1) ? seq->period.upper_inc : false;
			for (int j = 0; j < count; j++)
			{
				TSequence *seq1 = tnumberseq_at_range1(inst1, inst2, 
					lower_inc, upper_inc, linear, normranges[j]);
				if (seq1 != NULL) 
					result[k++] = seq1;
			}
			inst1 = inst2;
			lower_inc = true;
		}
		/* Stepwise sequence with inclusive upper bound must add a sequence for that bound */
		if (! linear && seq->period.upper_inc)
		{
			inst1 = tsequence_inst_n(seq, seq->count - 1);
			Datum value = tinstant_value(inst1);
			TypeCacheEntry *typcache = lookup_type_cache(
				normranges[count - 1]->rangetypid, TYPECACHE_RANGE_INFO);
			if (range_contains_elem_internal(typcache, normranges[count - 1], value))
				result[k++] = tsequence_make(&inst1, 1, true, true, STEP, NORMALIZE_NO);
		}
		if (k == 0)
			return 0;
		if (k > 1)
			tsequencearr_sort(result, k);
		return k;
	}
	else
	{
		/*
		 * MINUS function
		 * Compute first the tnumberseq_at_ranges, then compute its complement
		 */
		TSequenceSet *ts = tnumberseq_restrict_ranges(seq, normranges, count, REST_AT);
		if (ts == NULL)
		{
			result[0] = tsequence_copy(seq);
			return 1;
		}
		PeriodSet *ps1 = tsequenceset_get_time(ts);
		PeriodSet *ps2 = minus_period_periodset_internal(&seq->period, ps1);
		int newcount = 0;
		if (ps2 != NULL)
		{
			newcount = tsequence_at_periodset(result, seq, ps2);
			pfree(ps2);
		}
		pfree(ts); pfree(ps1); 
		return newcount;
	}
}

/**
 * Restricts the temporal number to (the complement of) the array 
 * of ranges of base values
 *
 * @param[in] seq Temporal number
 * @param[in] normranges Array of ranges of base values
 * @param[in] count Number of elements in the input array
 * @param[in] atfunc True when the restriction is at, false for minus 
 * @return Resulting temporal sequence set value
 * @pre The array of ranges is normalized
 */
TSequenceSet *
tnumberseq_restrict_ranges(const TSequence *seq, RangeType **normranges,
	int count, bool atfunc)
{
	int maxcount = seq->count * count;
	/* For minus and linear interpolation we need the double of the count */
	if (!atfunc && MOBDB_FLAGS_GET_LINEAR(seq->flags))
		maxcount *= 2;
	TSequence **sequences = palloc(sizeof(TSequence *) * maxcount);
	int newcount = tnumberseq_restrict_ranges1(sequences, seq, normranges,
		count, atfunc);
	return tsequenceset_make_free(sequences, newcount, NORMALIZE);
}

/**
 * Restricts the temporal value to (the complement of) the 
 * minimum/maximum base value
 */
TSequenceSet *
tsequence_restrict_minmax(const TSequence *seq, bool min, bool atfunc)
{
	Datum minmax = min ? tsequence_min_value(seq) : tsequence_max_value(seq);
	return tsequence_restrict_value(seq, minmax, atfunc);
}

/**
 * Restricts the temporal value to (the complement of) the maximum base value
 */
TSequenceSet *
tsequence_restrict_max(const TSequence *seq, bool atfunc)
{
	Datum max = tsequence_max_value(seq);
	return tsequence_restrict_value(seq, max, atfunc);
}

/**
 * Returns the base value of the segment of the temporal value at the 
 * timestamp
 *
 * @param[in] inst1,inst2 Temporal values defining the segment 
 * @param[in] linear True when the segment has linear interpolation
 * @param[in] t Timestamp
 * @pre The timestamp t is between inst1->t and inst2->t (both inclusive)
 * @note The function creates a new value that must be freed
 */
Datum
tsequence_value_at_timestamp1(const TInstant *inst1, const TInstant *inst2,
	bool linear, TimestampTz t)
{
	Oid valuetypid = inst1->valuetypid;
	Datum value1 = tinstant_value(inst1);
	Datum value2 = tinstant_value(inst2);
	/* Constant segment or t is equal to lower bound or step interpolation */
	if (datum_eq(value1, value2, valuetypid) ||
		inst1->t == t || (! linear && t < inst2->t))
		return tinstant_value_copy(inst1);

	/* t is equal to upper bound */
	if (inst2->t == t)
		return tinstant_value_copy(inst2);
	
	/* Interpolation for types with linear interpolation */
	double duration1 = (double) (t - inst1->t);
	double duration2 = (double) (inst2->t - inst1->t);
	double ratio = duration1 / duration2;
	Datum result = 0;
	ensure_linear_interpolation_all(valuetypid);
	if (valuetypid == FLOAT8OID)
	{ 
		double start = DatumGetFloat8(value1);
		double end = DatumGetFloat8(value2);
		double dresult = start + (end - start) * ratio;
		result = Float8GetDatum(dresult);
	}
	else if (valuetypid == type_oid(T_DOUBLE2))
	{
		double2 *start = DatumGetDouble2P(value1);
		double2 *end = DatumGetDouble2P(value2);
		double2 *dresult = palloc(sizeof(double2));
		dresult->a = start->a + (end->a - start->a) * ratio;
		dresult->b = start->b + (end->b - start->b) * ratio;
		result = Double2PGetDatum(dresult);
	}
	else if (valuetypid == type_oid(T_GEOMETRY))
	{
		result = geomseg_interpolate_point(value1, value2, ratio);
	}
	else if (valuetypid == type_oid(T_GEOGRAPHY))
	{
		result = geogseg_interpolate_point(value1, value2, ratio);
	}
	else if (valuetypid == type_oid(T_DOUBLE3))
	{
		double3 *start = DatumGetDouble3P(value1);
		double3 *end = DatumGetDouble3P(value2);
		double3 *dresult = palloc(sizeof(double3));
		dresult->a = start->a + (end->a - start->a) * ratio;
		dresult->b = start->b + (end->b - start->b) * ratio;
		dresult->c = start->c + (end->c - start->c) * ratio;
		result = Double3PGetDatum(dresult);
	}
	else if (valuetypid == type_oid(T_DOUBLE4))
	{
		double4 *start = DatumGetDouble4P(value1);
		double4 *end = DatumGetDouble4P(value2);
		double4 *dresult = palloc(sizeof(double4));
		dresult->a = start->a + (end->a - start->a) * ratio;
		dresult->b = start->b + (end->b - start->b) * ratio;
		dresult->c = start->c + (end->c - start->c) * ratio;
		dresult->d = start->d + (end->d - start->d) * ratio;
		result = Double4PGetDatum(dresult);
	}
	return result;
}

/**
 * Returns the base value of the temporal value at the timestamp
 *
 * @param[in] seq Temporal value
 * @param[in] t Timestamp
 * @param[out] result Base value
 * @result Returns true if the timestamp is contained in the temporal value
 */
bool
tsequence_value_at_timestamp(const TSequence *seq, TimestampTz t, Datum *result)
{
	/* Bounding box test */
	if (!contains_period_timestamp_internal(&seq->period, t))
		return false;

	/* Instantaneous sequence */
	if (seq->count == 1)
	{
		*result = tinstant_value_copy(tsequence_inst_n(seq, 0));
		return true;
	}

	/* General case */
	int n = tsequence_find_timestamp(seq, t);
	TInstant *inst1 = tsequence_inst_n(seq, n);
	TInstant *inst2 = tsequence_inst_n(seq, n + 1);
	*result = tsequence_value_at_timestamp1(inst1, inst2, MOBDB_FLAGS_GET_LINEAR(seq->flags), t);
	return true;
}

/**
 * Returns the base value of the temporal value at the timestamp when the
 * timestamp may be at an exclusive bound
 *
 * @param[in] seq Temporal value
 * @param[in] t Timestamp
 * @param[out] result Base value
 * @result Returns true if the timestamp is found in the temporal value
 */
bool
tsequence_value_at_timestamp_inc(const TSequence *seq, TimestampTz t, Datum *result)
{
	TInstant *inst = tsequence_inst_n(seq, 0);
	/* Instantaneous sequence or t is at lower bound */
	if (seq->count == 1 || inst->t == t)
		return tinstant_value_at_timestamp(inst, t, result);
	inst = tsequence_inst_n(seq, seq->count - 1);
	if (inst->t == t)
		return tinstant_value_at_timestamp(inst, t, result);
	return tsequence_value_at_timestamp(seq, t, result);
}

/**
 * Restricts the segment of a temporal value to the timestamp
 *
 * @param[in] inst1,inst2 Temporal values defining the segment 
 * @param[in] linear True when the segment has linear interpolation
 * @param[in] t Timestamp
 * @pre The timestamp t is between inst1->t and inst2->t (both inclusive)
 * @note The function creates a new value that must be freed
 */
TInstant *
tsequence_at_timestamp1(const TInstant *inst1, const TInstant *inst2,
	bool linear, TimestampTz t)
{
	Datum value = tsequence_value_at_timestamp1(inst1, inst2, linear, t);
	TInstant *result = tinstant_make(value, t, inst1->valuetypid);
	DATUM_FREE(value, inst1->valuetypid);
	return result;
}

/**
 * Restricts the temporal value to the timestamp
 */
TInstant *
tsequence_at_timestamp(const TSequence *seq, TimestampTz t)
{
	/* Bounding box test */
	if (!contains_period_timestamp_internal(&seq->period, t))
		return NULL;

	/* Instantaneous sequence */
	if (seq->count == 1)
		return tinstant_copy(tsequence_inst_n(seq, 0));
	
	/* General case */
	int n = tsequence_find_timestamp(seq, t);
	TInstant *inst1 = tsequence_inst_n(seq, n);
	TInstant *inst2 = tsequence_inst_n(seq, n + 1);
	return tsequence_at_timestamp1(inst1, inst2, MOBDB_FLAGS_GET_LINEAR(seq->flags), t);
}

/**
 * Restricts the temporal value to the complement of the timestamp
 *
 * @param[out] result Array on which the pointers of the newly constructed 
 * sequences are stored
 * @param[in] seq Temporal value
 * @param[in] t Timestamp
 * @return Number of resulting sequences returned
 * @note This function is called for each sequence of a temporal sequence set 
 */
int
tsequence_minus_timestamp1(TSequence **result, const TSequence *seq,
	TimestampTz t)
{
	/* Bounding box test */
	if (!contains_period_timestamp_internal(&seq->period, t))
	{
		result[0] = tsequence_copy(seq);
		return 1;
	}

	/* Instantaneous sequence */
	if (seq->count == 1)
		return 0;
	
	/* General case */
	bool linear = MOBDB_FLAGS_GET_LINEAR(seq->flags);
	TInstant **instants = palloc0(sizeof(TInstant *) * seq->count);
	int k = 0;
	int n = tsequence_find_timestamp(seq, t);
	TInstant *inst1 = tsequence_inst_n(seq, 0), *inst2;
	/* Compute the first sequence until t */
	if (n != 0 || inst1->t < t)
	{
		for (int i = 0; i < n; i++)
			instants[i] = tsequence_inst_n(seq, i);
		inst1 = tsequence_inst_n(seq, n);
		inst2 = tsequence_inst_n(seq, n + 1);
		if (inst1->t == t)
		{
			if (linear)
			{
				instants[n] = inst1;
				result[k++] = tsequence_make(instants, n + 1, 
					seq->period.lower_inc, false, linear, NORMALIZE_NO);
			}
			else
			{
				instants[n] = tinstant_make(tinstant_value(instants[n - 1]), t,
					inst1->valuetypid);
				result[k++] = tsequence_make(instants, n + 1, 
					seq->period.lower_inc, false, linear, NORMALIZE_NO);
				pfree(instants[n]);
			}
		}
		else
		{
			/* inst1->t < t */
			instants[n] = inst1;
			instants[n + 1] = linear ?
				tsequence_at_timestamp1(inst1, inst2, true, t) :
				tinstant_make(tinstant_value(inst1), t,
					inst1->valuetypid);
			result[k++] = tsequence_make(instants, n + 2, 
				seq->period.lower_inc, false, linear, NORMALIZE_NO);
			pfree(instants[n + 1]);
		}
	}
	/* Compute the second sequence after t */
	inst1 = tsequence_inst_n(seq, n);
	inst2 = tsequence_inst_n(seq, n + 1);
	if (t < inst2->t)
	{
		instants[0] = tsequence_at_timestamp1(inst1, inst2, linear, t);
		for (int i = 1; i < seq->count - n; i++)
			instants[i] = tsequence_inst_n(seq, i + n);
		result[k++] = tsequence_make(instants, seq->count - n, 
			false, seq->period.upper_inc, linear, NORMALIZE_NO);
		pfree(instants[0]);
	}
	return k;
}

/**
 * Restricts the temporal value to the complement of the timestamp
 *
 * @param[in] seq Temporal value
 * @param[in] t Timestamp
 * @return Resulting temporal sequence set 
 */
TSequenceSet *
tsequence_minus_timestamp(const TSequence *seq, TimestampTz t)
{
	TSequence *sequences[2];
	int count = tsequence_minus_timestamp1((TSequence **)sequences, seq, t);
	if (count == 0)
		return NULL;
	TSequenceSet *result = tsequenceset_make(sequences, count, NORMALIZE_NO);
	for (int i = 0; i < count; i++)
		pfree(sequences[i]);
	return result;
}

/**
 * Restricts the temporal value to the timestamp set
 */
TInstantSet *
tsequence_at_timestampset(const TSequence *seq, const TimestampSet *ts)
{
	/* Bounding box test */
	Period *p = timestampset_bbox(ts);
	if (!overlaps_period_period_internal(&seq->period, p))
		return NULL;
	
	/* Instantaneous sequence */
	TInstant *inst = tsequence_inst_n(seq, 0);
	if (seq->count == 1)
	{
		if (!contains_timestampset_timestamp_internal(ts, inst->t))
			return NULL;
		return tinstantset_make(&inst, 1);
	}

	/* General case */
	TimestampTz t = Max(seq->period.lower, p->lower);
	int loc;
	timestampset_find_timestamp(ts, t, &loc);
	TInstant **instants = palloc(sizeof(TInstant *) * (ts->count - loc));
	int k = 0;
	for (int i = loc; i < ts->count; i++) 
	{
		t = timestampset_time_n(ts, i);
		inst = tsequence_at_timestamp(seq, t);
		if (inst != NULL)
			instants[k++] = inst;
	}
	return tinstantset_make_free(instants, k);
}

/**
 * Restricts the temporal value to the complement of the timestamp set
 *
 * @param[out] result Array on which the pointers of the newly constructed 
 * sequences are stored
 * @param[in] seq Temporal value
 * @param[in] ts Timestampset
 * @return Number of resulting sequences returned
 */
int
tsequence_minus_timestampset1(TSequence **result, const TSequence *seq,
	const TimestampSet *ts)
{
	/* Bounding box test */
	Period *p = timestampset_bbox(ts);
	if (!overlaps_period_period_internal(&seq->period, p))
	{
		result[0] = tsequence_copy(seq);
		return 1;
	}

	/* Instantaneous sequence */
	if (seq->count == 1)
	{
		TInstant *inst = tsequence_inst_n(seq, 0);
		if (contains_timestampset_timestamp_internal(ts,inst->t))
			return 0;
		result[0] = tsequence_copy(seq);
		return 1;
	}

	/* Instantaneous timestamp set */
	if (ts->count == 1)
	{
		return tsequence_minus_timestamp1(result, seq,
			timestampset_time_n(ts, 0));
	}

	/* General case */
	bool linear = MOBDB_FLAGS_GET_LINEAR(seq->flags);
	TInstant **instants = palloc0(sizeof(TInstant *) * seq->count);
	TInstant *inst, *tofree = NULL;
	instants[0] = tsequence_inst_n(seq, 0);
	int i = 1,	/* current instant of the argument sequence */
		j = 0,	/* current timestamp of the argument timestamp set */
		k = 0,	/* current number of new sequences */
		l = 1;	/* number of instants in the currently constructed sequence */
	bool lower_inc = seq->period.lower_inc;
	while (i < seq->count && j < ts->count)
	{
		inst = tsequence_inst_n(seq, i);
		TimestampTz t = timestampset_time_n(ts, j);
		if (inst->t < t)
		{
			instants[l++] = inst;
			i++; /* advance instants */
		}
		else if (inst->t == t)
		{
			if (linear)
			{
				instants[l] = inst;
				result[k++] = tsequence_make(instants, l + 1,
					lower_inc, false, linear, NORMALIZE_NO);
				instants[0] = inst;
			}
			else
			{
				instants[l] = tinstant_make(tinstant_value(instants[l - 1]),
					t, inst->valuetypid);
				result[k++] = tsequence_make(instants, l + 1,
					lower_inc, false, linear, NORMALIZE_NO);
				pfree(instants[l]);
				if (tofree)
				{
					pfree(tofree);
					tofree = NULL;
				}
				instants[0] = inst;
			}
			l = 1;
			lower_inc = false;
			i++; /* advance instants */
			j++; /* advance timestamps */
		}
		else
		{
			/* inst->t > t */
			if (instants[l - 1]->t < t)
			{
				/* The instant to remove is not the first one of the sequence */
				instants[l] = linear ?
					tsequence_at_timestamp1(instants[l - 1], inst, true, t) :
					tinstant_make(tinstant_value(instants[l - 1]), t,
						inst->valuetypid);
				result[k++] = tsequence_make(instants, l + 1,
					lower_inc, false, linear, NORMALIZE_NO);
				if (tofree)
					pfree(tofree);
				instants[0] = tofree = instants[l];
				l = 1;
			}
			lower_inc = false;
			j++; /* advance timestamps */
		}
	}
	/* Compute the sequence after the timestamp set */
	if (i < seq->count)
	{
		for (j = i; j < seq->count; j++)
			instants[l++] = tsequence_inst_n(seq, j);
		result[k++] = tsequence_make(instants, l,
			false, seq->period.upper_inc, linear, NORMALIZE_NO);
	}
	if (tofree)
		pfree(tofree);
	return k;
}

/**
 * Restricts the temporal value to the complement of the timestamp set
 */
TSequenceSet *
tsequence_minus_timestampset(const TSequence *seq, const TimestampSet *ts)
{
	TSequence **sequences = palloc0(sizeof(TSequence *) * (ts->count + 1));
	int count = tsequence_minus_timestampset1(sequences, seq, ts);
	return tsequenceset_make_free(sequences, count, NORMALIZE);
}

/**
 * Restricts the temporal value to the period
 */
TSequence *
tsequence_at_period(const TSequence *seq, const Period *p)
{
	/* Bounding box test */
	if (!overlaps_period_period_internal(&seq->period, p))
		return NULL;

	/* Instantaneous sequence */
	if (seq->count == 1)
		return tsequence_copy(seq);

	/* General case */
	Period *inter = intersection_period_period_internal(&seq->period, p);
	bool linear = MOBDB_FLAGS_GET_LINEAR(seq->flags);
	TSequence *result;
	/* Intersecting period is instantaneous */
	if (inter->lower == inter->upper)
	{
		TInstant *inst = tsequence_at_timestamp(seq, inter->lower);
		result = tsequence_make(&inst, 1, true, true, linear, NORMALIZE_NO);
		pfree(inst); pfree(inter);
		return result;
	}
	
	int n = tsequence_find_timestamp(seq, inter->lower);
	/* If the lower bound of the intersecting period is exclusive */
	if (n == -1)
		n = 0;
	TInstant **instants = palloc(sizeof(TInstant *) * (seq->count - n));
	/* Compute the value at the beginning of the intersecting period */
	TInstant *inst1 = tsequence_inst_n(seq, n);
	TInstant *inst2 = tsequence_inst_n(seq, n + 1);
	instants[0] = tsequence_at_timestamp1(inst1, inst2, linear, inter->lower);
	int k = 1;
	for (int i = n + 2; i < seq->count; i++)
	{
		/* If the end of the intersecting period is between inst1 and inst2 */
		if (inst1->t <= inter->upper && inter->upper <= inst2->t)
			break;

		inst1 = inst2;
		inst2 = tsequence_inst_n(seq, i);
		/* If the intersecting period contains inst1 */
		if (inter->lower <= inst1->t && inst1->t <= inter->upper)
			instants[k++] = inst1;
	}
	/* The last two values of sequences with step interpolation and
	   exclusive upper bound must be equal */
	if (linear || inter->upper_inc)
		instants[k++] = tsequence_at_timestamp1(inst1, inst2, linear,
			inter->upper);
	else
	{	
		Datum value = tinstant_value(instants[k - 1]);
		instants[k++] = tinstant_make(value, inter->upper, seq->valuetypid);
	}
	/* Since by definition the sequence is normalized it is not necessary to
	   normalize the projection of the sequence to the period */
	result = tsequence_make(instants, k, inter->lower_inc, inter->upper_inc,
		linear, NORMALIZE_NO);

	pfree(instants[0]); pfree(instants[k - 1]); pfree(instants); pfree(inter);
	
	return result;
}

/**
 * Restricts the temporal value to the complement of the period
 *
 * @param[out] result Array on which the pointers of the newly constructed 
 * sequences are stored
 * @param[in] seq Temporal value
 * @param[in] p Period
 * @return Number of resulting sequences returned
 */
int
tsequence_minus_period1(TSequence **result, const TSequence *seq,
	const Period *p)
{
	/* Bounding box test */
	if (!overlaps_period_period_internal(&seq->period, p))
	{
		result[0] = tsequence_copy(seq);
		return 1;
	}
	
	/* Instantaneous sequence */
	if (seq->count == 1)
		return 0;

	/* General case */
	PeriodSet *ps = minus_period_period_internal(&seq->period, p);
	if (ps == NULL)
		return 0;
	for (int i = 0; i < ps->count; i++)
	{
		Period *p1 = periodset_per_n(ps, i);
		result[i] = tsequence_at_period(seq, p1);
	}
	pfree(ps);
	return ps->count;
}

/**
 * Restricts the temporal value to the complement of the period
 */
TSequenceSet *
tsequence_minus_period(const TSequence *seq, const Period *p)
{
	TSequence *sequences[2];
	int count = tsequence_minus_period1(sequences, seq, p);
	if (count == 0)
		return NULL;
	TSequenceSet *result = tsequenceset_make(sequences, count, NORMALIZE_NO);
	for (int i = 0; i < count; i++)
		pfree(sequences[i]);
	return result;
}

/**
 * Restricts the temporal value to the period set
 
 * @param[out] result Array on which the pointers of the newly constructed 
 * sequences are stored
 * @param[in] seq Temporal value
 * @param[in] ps Period set
 * @return Number of resulting sequences returned
 * @note This function is called for each sequence of a temporal sequence set
*/
int
tsequence_at_periodset(TSequence **result, const TSequence *seq, 
	const PeriodSet *ps)
{
	/* Bounding box test */
	Period *p = periodset_bbox(ps);
	if (!overlaps_period_period_internal(&seq->period, p))
		return 0;

	/* Instantaneous sequence */
	if (seq->count == 1)
	{
		TInstant *inst = tsequence_inst_n(seq, 0);
		if (!contains_periodset_timestamp_internal(ps, inst->t))
			return 0;
		result[0] = tsequence_copy(seq);
		return 1;
	}

	/* General case */
	int loc;
	periodset_find_timestamp(ps, seq->period.lower, &loc);
	int k = 0;
	for (int i = loc; i < ps->count; i++)
	{
		p = periodset_per_n(ps, i);
		TSequence *seq1 = tsequence_at_period(seq, p);
		if (seq1 != NULL)
			result[k++] = seq1;
		if (seq->period.upper < p->upper)
			break;
	}
	return k;
}

/**
 * Restricts the temporal value to the complement of the period set
 *
 * @param[out] result Array on which the pointers of the newly constructed 
 * sequences are stored
 * @param[in] seq Temporal value
 * @param[in] ps Period set
 * @param[in] from Index from which the processing starts
 * @return Number of resulting sequences returned
 * @note This function is called for each sequence of a temporal sequence set
*/
int
tsequence_minus_periodset(TSequence **result, const TSequence *seq,
	const PeriodSet *ps, int from)
{
	/* The sequence can be split at most into (count + 1) sequences
		|----------------------|
			|---| |---| |---|
	*/
	TSequence *curr = tsequence_copy(seq);
	int k = 0;
	for (int i = from; i < ps->count; i++)
	{
		Period *p1 = periodset_per_n(ps, i);
		/* If the remaining periods are to the left of the current period */
		int cmp = timestamp_cmp_internal(curr->period.upper, p1->lower);
		if (cmp < 0 || (cmp == 0 && curr->period.upper_inc && ! p1->lower_inc))
		{
			result[k++] = curr;
			break;
		}
		TSequence *minus[2];
		int countminus = tsequence_minus_period1(minus, curr, p1);
		pfree(curr);
		/* minus can have from 0 to 2 periods */
		if (countminus == 0)
			break;
		else if (countminus == 1)
			curr = minus[0];
		else /* countminus == 2 */
		{
			result[k++] = minus[0];
			curr = minus[1];
		}
		/* There are no more periods left */
		if (i == ps->count - 1)
			result[k++] = curr;
	}
	return k;
}

/**
 * Restricts the temporal value to the (complement of the) period set
 *
 * @param[in] seq Temporal value
 * @param[in] ps Period set
 * @param[in] atfunc True when the restriction is at, false for minus 
 * @return Resulting temporal sequence set
 */
TSequenceSet *
tsequence_restrict_periodset(const TSequence *seq, const PeriodSet *ps, bool atfunc)
{
	/* Bounding box test */
	Period *p = periodset_bbox(ps);
	if (!overlaps_period_period_internal(&seq->period, p))
		return atfunc ? NULL : tsequence_to_tsequenceset(seq);

	/* Instantaneous sequence */
	if (seq->count == 1)
	{
		TInstant *inst = tsequence_inst_n(seq, 0);
		if (contains_periodset_timestamp_internal(ps, inst->t))
			return atfunc ? tsequence_to_tsequenceset(seq) : NULL;
		return atfunc ? NULL : tsequence_to_tsequenceset(seq);
	}

	/* General case */
	if (atfunc)
	{
		TSequence **sequences = palloc(sizeof(TSequence *) * ps->count);
		int count = tsequence_at_periodset(sequences, seq, ps);
		return tsequenceset_make_free(sequences, count, NORMALIZE);
	}
	else
	{
		TSequence **sequences = palloc(sizeof(TSequence *) * (ps->count + 1));
		int count = tsequence_minus_periodset(sequences, seq, ps, 0);
		return tsequenceset_make_free(sequences, count, NORMALIZE_NO);
	}
}

/*****************************************************************************
 * Intersects functions
 *****************************************************************************/

/**
 * Returns true if the temporal value intersects the timestamp
 */
bool
tsequence_intersects_timestamp(const TSequence *seq, TimestampTz t)
{
	return contains_period_timestamp_internal(&seq->period, t);
}

/**
 * Returns true if the temporal value intersects the timestamp set
 */
bool
tsequence_intersects_timestampset(const TSequence *seq, const TimestampSet *ts)
{
	for (int i = 0; i < ts->count; i++)
		if (tsequence_intersects_timestamp(seq, timestampset_time_n(ts, i))) 
			return true;
	return false;
}

/**
 * Returns true if the temporal value intersects the period
 */
bool
tsequence_intersects_period(const TSequence *seq, const Period *p)
{
	return overlaps_period_period_internal(&seq->period, p);
}

/**
 * Returns true if the temporal value intersects the period set
 */
bool
tsequence_intersects_periodset(const TSequence *seq, const PeriodSet *ps)
{
	for (int i = 0; i < ps->count; i++)
		if (tsequence_intersects_period(seq, periodset_per_n(ps, i))) 
			return true;
	return false;
}

/*****************************************************************************
 * Local aggregate functions 
 *****************************************************************************/

/**
 * Returns the integral (area under the curve) of the temporal number
 */
double
tnumberseq_integral(const TSequence *seq)
{
	double result = 0;
	TInstant *inst1 = tsequence_inst_n(seq, 0);
	for (int i = 1; i < seq->count; i++)
	{
		TInstant *inst2 = tsequence_inst_n(seq, i);
		if (MOBDB_FLAGS_GET_LINEAR(seq->flags))
		{
			/* Linear interpolation */
			double min = Min(DatumGetFloat8(tinstant_value(inst1)), 
				DatumGetFloat8(tinstant_value(inst2)));
			double max = Max(DatumGetFloat8(tinstant_value(inst1)), 
				DatumGetFloat8(tinstant_value(inst2)));
			result += (max + min) * (double) (inst2->t - inst1->t) / 2.0;
		}
		else
		{
			/* Step interpolation */
			result += datum_double(tinstant_value(inst1), inst1->valuetypid) *
				(double) (inst2->t - inst1->t);
		}
		inst1 = inst2;
	}
	return result;
}

/**
 * Returns the time-weighted average of the temporal number
 */
double
tnumberseq_twavg(const TSequence *seq)
{
	double duration = (double) (seq->period.upper - seq->period.lower);
	double result;
	if (duration == 0.0)
		/* Instantaneous sequence */
		result = datum_double(tinstant_value(tsequence_inst_n(seq, 0)),
			seq->valuetypid);
	else
		result = tnumberseq_integral(seq) / duration;
	return result;
}

/*****************************************************************************
 * Functions for defining B-tree indexes
 *****************************************************************************/

/**
 * Returns true if the two temporal sequence values are equal
 *
 * @pre The arguments are of the same base type
 * @note The internal B-tree comparator is not used to increase efficiency
 */
bool
tsequence_eq(const TSequence *seq1, const TSequence *seq2)
{
	assert(seq1->valuetypid == seq2->valuetypid);
	/* If number of sequences, flags, or periods are not equal */
	if (seq1->count != seq2->count || seq1->flags != seq2->flags ||
			! period_eq_internal(&seq1->period, &seq2->period)) 
		return false;

	/* If bounding boxes are not equal */
	void *box1 = tsequence_bbox_ptr(seq1);
	void *box2 = tsequence_bbox_ptr(seq2);
	if (! temporal_bbox_eq(box1, box2, seq1->valuetypid))
		return false;
	
	/* Compare the composing instants */
	for (int i = 0; i < seq1->count; i++)
	{
		TInstant *inst1 = tsequence_inst_n(seq1, i);
		TInstant *inst2 = tsequence_inst_n(seq2, i);
		if (! tinstant_eq(inst1, inst2))
			return false;
	}
	return true;
}

/**
 * Returns -1, 0, or 1 depending on whether the first temporal value 
 * is less than, equal, or greater than the second one
 *
 * @pre The arguments are of the same base type
 */
int
tsequence_cmp(const TSequence *seq1, const TSequence *seq2)
{
	assert(seq1->valuetypid == seq2->valuetypid);
	
	/* Compare periods
	 * We need to compare periods AND bounding boxes since the bounding boxes
	 * do not distinguish between inclusive and exclusive bounds */
	int result = period_cmp_internal(&seq1->period, &seq2->period);
	if (result)
		return result;
	
	/* Compare bounding box */
	bboxunion box1, box2;
	memset(&box1, 0, sizeof(bboxunion));
	memset(&box2, 0, sizeof(bboxunion));
	tsequence_bbox(&box1, seq1);
	tsequence_bbox(&box2, seq2);
	result = temporal_bbox_cmp(&box1, &box2, seq1->valuetypid);
	if (result)
		return result;

	/* Compare composing instants */
	int count = Min(seq1->count, seq2->count);
	for (int i = 0; i < count; i++)
	{
		TInstant *inst1 = tsequence_inst_n(seq1, i);
		TInstant *inst2 = tsequence_inst_n(seq2, i);
		result = tinstant_cmp(inst1, inst2);
		if (result) 
			return result;
	}

	/* seq1->count == seq2->count because of the bounding box and the
	 * composing instant tests above */

	/* Compare flags  */
	if (seq1->flags < seq2->flags)
		return -1;
	if (seq1->flags > seq2->flags)
		return 1;

	/* The two values are equal */
	return 0;
}

/*****************************************************************************
 * Function for defining hash index
 * The function reuses the approach for array types for combining the hash of  
 * the elements and the approach for range types for combining the period 
 * bounds.
 *****************************************************************************/

/**
 * Returns the hash value of the temporal value
 */
uint32
tsequence_hash(const TSequence *seq)
{
	uint32 result;
	char flags = '\0';

	/* Create flags from the lower_inc and upper_inc values */
	if (seq->period.lower_inc)
		flags |= 0x01;
	if (seq->period.upper_inc)
		flags |= 0x02;
	result = DatumGetUInt32(hash_uint32((uint32) flags));
	
	/* Merge with hash of instants */
	for (int i = 0; i < seq->count; i++)
	{
		TInstant *inst = tsequence_inst_n(seq, i);
		uint32 inst_hash = tinstant_hash(inst);
		result = (result << 5) - result + inst_hash;
	}
	return result;
}

/*****************************************************************************/
