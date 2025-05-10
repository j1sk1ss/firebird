/*
 *      PROGRAM:        JRD Access Method
 *      MODULE:         evl.cpp
 *      DESCRIPTION:    Expression evaluation
 *
 * The contents of this file are subject to the Interbase Public
 * License Version 1.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy
 * of the License at http://www.Inprise.com/IPL.html
 *
 * Software distributed under the License is distributed on an
 * "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either express
 * or implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code was created by Inprise Corporation
 * and its predecessors. Portions created by Inprise Corporation are
 * Copyright (C) Inprise Corporation.
 *
 * All Rights Reserved.
 * Contributor(s): ______________________________________.
 */

/*
 * Modified by: Patrick J. P. Griffin
 * Date: 11/24/2000
 * Problem:   select count(0)+1 from rdb$relations where 0=1; returns 0
 *            In the EVL_group processing, the internal assigment for
 *            the literal in the computation is being done on every
 *            statement fetch, so if there are no statements fetched
 *            then the internal field never gets set.
 * Change:    Added an assignment process for the literal
 *            before the first fetch.
 *
 * Modified by: Neil McCalden
 * Date: 05 Jan 2001
 * Problem:   Firebird bug: 127375
 *            Group by on a calculated expression would cause segv
 *            when it encountered a NULL value as the calculation
 *            was trying reference a null pointer.
 * Change:    Test the null flag before trying to expand the value.
 *
 * 2001.6.17 Claudio Valderrama: Fix the annoying behavior that causes silent
 *	overflow in dialect 1. If you define the macro FIREBIRD_AVOID_DIALECT1_OVERFLOW
 *	it will work with double should an overflow happen. Otherwise, an error will be
 *	issued to the user if the overflow happens. The multiplication is done using
 *	SINT64 quantities. I had the impression that casting this SINT64 result to double
 *	when we detect overflow was faster than achieving the failed full multiplication
 *	with double operands again. Usage will tell the truth.
 *	For now, the aforementioned macro is enabled.
 * 2001.6.18 Claudio Valderrama: substring() is working with international charsets,
 *	thanks to Dave Schnepper's directions.
 * 2002.2.15 Claudio Valderrama: divide2() should not mangle negative values.
 * 2002.04.16 Paul Beach HP10 Port - (UCHAR*) desc.dsc_address = p; modified for HP
 *	Compiler
 * 2002.09.28 Dmitry Yemanov: Reworked internal_info stuff, enhanced
 *                            exception handling in SPs/triggers,
 *                            implemented ROWS_AFFECTED system variable
 * 2003.08.10 Claudio Valderrama: Fix SF bug# 784121.
 * Adriano dos Santos Fernandes
 */

#include "firebird.h"
#include <string.h>
#include <math.h>
#include "../dsql/Nodes.h"
#include "../dsql/ExprNodes.h"
#include "../dsql/StmtNodes.h"
#include "../jrd/jrd.h"
#include "../jrd/val.h"
#include "../jrd/req.h"
#include "../jrd/exe.h"
#include "../jrd/sbm.h"
#include "../jrd/blb.h"
#include "iberror.h"
#include "../jrd/scl.h"
#include "../jrd/lck.h"
#include "../jrd/lls.h"
#include "../jrd/intl.h"
#include "../jrd/intl_classes.h"
#include "../jrd/sort.h"
#include "firebird/impl/blr.h"
#include "../jrd/tra.h"
#include "../common/gdsassert.h"
#include "../common/classes/auto.h"
#include "../common/classes/timestamp.h"
#include "../common/classes/VaryStr.h"
#include "../jrd/blb_proto.h"
#include "../jrd/btr_proto.h"
#include "../jrd/cvt_proto.h"
#include "../jrd/DataTypeUtil.h"
#include "../jrd/dpm_proto.h"
#include "../common/dsc_proto.h"
#include "../jrd/err_proto.h"
#include "../jrd/evl_proto.h"
#include "../jrd/exe_proto.h"
#include "../jrd/fun_proto.h"
#include "../jrd/intl_proto.h"
#include "../jrd/lck_proto.h"
#include "../jrd/met_proto.h"
#include "../jrd/mov_proto.h"
#include "../jrd/pag_proto.h"
#include "../jrd/rlck_proto.h"
#include "../jrd/scl_proto.h"
#include "../yvalve/gds_proto.h"
#include "../jrd/align.h"
#include "../jrd/met_proto.h"
#include "../common/config/config.h"
#include "../jrd/SysFunction.h"
#include "../jrd/recsrc/RecordSource.h"
#include "../jrd/recsrc/Cursor.h"
#include "../common/classes/Aligner.h"
#include "../jrd/Function.h"

using namespace Jrd;
using namespace Firebird;


dsc* EVL_assign_to(thread_db* tdbb, const ValueExprNode* node)
{
/**************************************
 *
 *      E V L _ a s s i g n _ t o
 *
 **************************************
 *
 * Functional description
 *      Evaluate the descriptor of the
 *      destination node of an assignment.
 *
 **************************************/

	SET_TDBB(tdbb);

	DEV_BLKCHK(node, type_nod);

	Request* request = tdbb->getRequest();

	// The only nodes that can be assigned to are: argument, field and variable.

	if (auto paramNode = nodeAs<ParameterNode>(node))
	{
		auto paramRequest = paramNode->getParamRequest(request);
		auto message = paramNode->message;
		auto arg_number = paramNode->argNumber;
		auto desc = &message->getFormat(paramRequest)->fmt_desc[arg_number];

		auto impure = request->getImpure<impure_value>(node->impureOffset);

		impure->vlu_desc.dsc_address = message->getBuffer(paramRequest) + (IPTR) desc->dsc_address;
		impure->vlu_desc.dsc_dtype = desc->dsc_dtype;
		impure->vlu_desc.dsc_length = desc->dsc_length;
		impure->vlu_desc.dsc_scale = desc->dsc_scale;
		impure->vlu_desc.dsc_sub_type = desc->dsc_sub_type;

		if (DTYPE_IS_TEXT(desc->dsc_dtype) &&
			((INTL_TTYPE(desc) == ttype_dynamic) || (INTL_GET_CHARSET(desc) == CS_dynamic)))
		{
			// Value is a text value, we're assigning it back to the user
			// process, user process has not specified a subtype, user
			// process specified dynamic translation and the dsc isn't from
			// a 3.3 type request (blr_cstring2 instead of blr_cstring) so
			// convert the charset to the declared charset of the process.

			impure->vlu_desc.setTextType(tdbb->getCharSet());
		}

		return &impure->vlu_desc;
	}
	else if (nodeIs<NullNode>(node))
		return NULL;
	else if (auto varNode = nodeAs<VariableNode>(node))
	{
		auto impure = varNode->getVarRequest(request)->getImpure<impure_value>(varNode->varDecl->impureOffset);
		return &impure->vlu_desc;
	}
	else if (auto fieldNode = nodeAs<FieldNode>(node))
	{
		auto record = request->req_rpb[fieldNode->fieldStream].rpb_record;
		auto impure = request->getImpure<impure_value>(node->impureOffset);

		if (!EVL_field(0, record, fieldNode->fieldId, &impure->vlu_desc))
		{
			// The below condition means that EVL_field() returned
			// a read-only dummy value which cannot be assigned to.
			// The usual reason is a field being unexpectedly dropped.
			if (impure->vlu_desc.dsc_address && !(impure->vlu_desc.dsc_flags & DSC_null))
				ERR_post(Arg::Gds(isc_field_disappeared));
		}

		if (!impure->vlu_desc.dsc_address)
			ERR_post(Arg::Gds(isc_read_only_field) << "<unknown>");

		return &impure->vlu_desc;
	}

	SOFT_BUGCHECK(229);	// msg 229 EVL_assign_to: invalid operation
	return NULL;
}


RecordBitmap** EVL_bitmap(thread_db* tdbb, const InversionNode* node, RecordBitmap* bitmap_and)
{
/**************************************
 *
 *      E V L _ b i t m a p
 *
 **************************************
 *
 * Functional description
 *      Evaluate bitmap valued expression.
 *
 **************************************/

	SET_TDBB(tdbb);

	DEV_BLKCHK(node, type_nod);

	JRD_reschedule(tdbb);

	switch (node->type)
	{
	case InversionNode::TYPE_AND:
		{
			RecordBitmap** bitmap = EVL_bitmap(tdbb, node->node1, bitmap_and);
			if (!(*bitmap) || !(*bitmap)->getFirst())
				return bitmap;

			return EVL_bitmap(tdbb, node->node2, *bitmap);
		}

	case InversionNode::TYPE_OR:
		return RecordBitmap::bit_or(
			EVL_bitmap(tdbb, node->node1, bitmap_and),
			EVL_bitmap(tdbb, node->node2, bitmap_and));

	case InversionNode::TYPE_IN:
		{
			RecordBitmap** inv_bitmap = EVL_bitmap(tdbb, node->node1, bitmap_and);
			BTR_evaluate(tdbb, node->node2->retrieval, inv_bitmap, bitmap_and);
			return inv_bitmap;
		}

	case InversionNode::TYPE_DBKEY:
		{
			Request* request = tdbb->getRequest();
			impure_inversion* impure = request->getImpure<impure_inversion>(node->impure);
			RecordBitmap::reset(impure->inv_bitmap);
			const dsc* const desc = EVL_expr(tdbb, request, node->value);

			if (!(tdbb->getRequest()->req_flags & req_null) &&
				(desc->isText() || desc->isDbKey()))
			{
				UCHAR* ptr = NULL;
				const int length = MOV_get_string(tdbb, desc, &ptr, NULL, 0);

				if (length == sizeof(RecordNumber::Packed))
				{
					const USHORT id = node->id;
					Aligner<RecordNumber::Packed> alignedNumbers(ptr, length);
					const RecordNumber::Packed* numbers = alignedNumbers;
					RecordNumber rel_dbkey;
					rel_dbkey.bid_decode(&numbers[id]);
					// Decrement the value in order to switch back to the zero based numbering
					// (from the user point of view the DB_KEY numbering starts from one)
					rel_dbkey.decrement();
					if (!bitmap_and || bitmap_and->test(rel_dbkey.getValue()))
						RBM_SET(tdbb->getDefaultPool(), &impure->inv_bitmap, rel_dbkey.getValue());
				}
			}

			return &impure->inv_bitmap;
		}

	case InversionNode::TYPE_INDEX:
		{
			impure_inversion* impure = tdbb->getRequest()->getImpure<impure_inversion>(node->impure);
			RecordBitmap::reset(impure->inv_bitmap);
			BTR_evaluate(tdbb, node->retrieval, &impure->inv_bitmap, bitmap_and);
			return &impure->inv_bitmap;
		}

	default:
		SOFT_BUGCHECK(230);			// msg 230 EVL_bitmap: invalid operation
	}

	return NULL;
}


void EVL_dbkey_bounds(thread_db* tdbb, const Array<DbKeyRangeNode*>& ranges,
	jrd_rel* relation, RecordNumber& lowerBound, RecordNumber& upperBound)
{
/**************************************
*
*      E V L _ d b k e y _ b o u n d s
*
**************************************
*
* Functional description
*      Evaluate DBKEY ranges and find lower/upper bounds.
*
**************************************/
	SET_TDBB(tdbb);

	Request* const request = tdbb->getRequest();

	for (const auto node : ranges)
	{
		if (node->lower)
		{
			const auto desc = EVL_expr(tdbb, request, node->lower);

			if (!(request->req_flags & req_null) &&
				desc && (desc->isText() || desc->isDbKey()))
			{
				UCHAR* ptr = NULL;
				const auto length = MOV_get_string(tdbb, desc, &ptr, NULL, 0);

				if (length == sizeof(RecordNumber::Packed))
				{
					Aligner<RecordNumber::Packed> alignedNumber(ptr, length);
					const auto dbkey = (const RecordNumber::Packed*) alignedNumber;

					if (dbkey->bid_relation_id == relation->rel_id)
					{
						RecordNumber recno;
						recno.bid_decode(dbkey);
						// Decrement the value in order to switch back to the zero based numbering
						recno.decrement();
						recno.setValid(true);

						if ((!lowerBound.isValid() || recno > lowerBound) &&
							recno.getValue() > BOF_NUMBER)
						{
							lowerBound = recno;
						}
					}
				}
			}
		}

		if (node->upper)
		{
			const auto desc = EVL_expr(tdbb, request, node->upper);

			if (!(request->req_flags & req_null) &&
				desc && (desc->isText() || desc->isDbKey()))
			{
				UCHAR* ptr = NULL;
				const auto length = MOV_get_string(tdbb, desc, &ptr, NULL, 0);

				if (length == sizeof(RecordNumber::Packed))
				{
					Aligner<RecordNumber::Packed> alignedNumber(ptr, length);
					const auto dbkey = (const RecordNumber::Packed*) alignedNumber;

					if (dbkey->bid_relation_id == relation->rel_id)
					{
						RecordNumber recno;
						recno.bid_decode(dbkey);
						// Decrement the value in order to switch back to the zero based numbering
						recno.decrement();
						recno.setValid(true);

						if ((!upperBound.isValid() || recno < upperBound) &&
							recno.getValue() > BOF_NUMBER)
						{
							upperBound = recno;
						}
					}
				}
			}
		}
	}
}


bool EVL_field(jrd_rel* relation, Record* record, USHORT id, dsc* desc)
{
/**************************************
 *
 *      E V L _ f i e l d
 *
 **************************************
 *
 * Functional description
 *      Evaluate a field by filling out a descriptor.
 *
 **************************************/

	DEV_BLKCHK(record, type_rec);

	if (!record)
	{
		// ASF: Usage of ERR_warning with Arg::Gds (instead of Arg::Warning) is correct here.
		// Maybe not all code paths are prepared for throwing an exception here,
		// but it will leave the engine as an error (when testing for req_warning).
		ERR_warning(Arg::Gds(isc_no_cur_rec));
		return false;
	}

	const Format* format = record->getFormat();
	fb_assert(format);

	if (id < format->fmt_count)
		*desc = format->fmt_desc[id];

	if (id >= format->fmt_count || desc->isUnknown())
	{
		// Map a non-existent field to a default value, if available.
		// This enables automatic format upgrade for data rows.
		// Reference: Bug 10424, 10116

		if (relation)
		{
			thread_db* tdbb = JRD_get_thread_data();

			const Format* const currentFormat = MET_current(tdbb, relation);

			while (id >= format->fmt_defaults.getCount() ||
				 format->fmt_defaults[id].vlu_desc.isUnknown())
			{
				if (format->fmt_version >= currentFormat->fmt_version)
				{
					format = NULL;
					break;
				}

				format = MET_format(tdbb, relation, format->fmt_version + 1);
				fb_assert(format);
			}

			if (format)
			{
				*desc = format->fmt_defaults[id].vlu_desc;

				if (record->isNull())
					desc->dsc_flags |= DSC_null;

				return !(desc->dsc_flags & DSC_null);
			}
		}

		desc->makeText(1, ttype_ascii, (UCHAR*) " ");
		return false;
	}

	// If the offset of the field is 0, the field can't possible exist

	if (!desc->dsc_address)
		return false;

	desc->dsc_address = record->getData() + (IPTR) desc->dsc_address;

	if (record->isNull(id))
	{
		desc->dsc_flags |= DSC_null;
		return false;
	}

	desc->dsc_flags &= ~DSC_null;
	return true;
}


void EVL_make_value(thread_db* tdbb, const dsc* desc, impure_value* value, MemoryPool* pool)
{
/**************************************
 *
 *      E V L _ m a k e _ v a l u e
 *
 **************************************
 *
 * Functional description
 *      Make a value block reflect the value of a descriptor.
 *
 **************************************/
	SET_TDBB(tdbb);

	// Handle the fixed length data types first.  They're easy.

	const dsc from = *desc;
	value->vlu_desc = *desc;
	value->vlu_desc.dsc_address = (UCHAR*) &value->vlu_misc;

	switch (from.dsc_dtype)
	{
	case dtype_short:
		value->vlu_misc.vlu_short = *((SSHORT*) from.dsc_address);
		return;

	case dtype_long:
	case dtype_real:
		value->vlu_misc.vlu_long = *((SLONG*) from.dsc_address);
		return;

	case dtype_int64:
		value->vlu_misc.vlu_int64 = *((SINT64*) from.dsc_address);
		return;

	case dtype_double:
		value->vlu_misc.vlu_double = *((double*) from.dsc_address);
		return;

	case dtype_dec64:
		value->vlu_misc.vlu_dec64 = *((Decimal64*) from.dsc_address);
		return;

	case dtype_dec128:
		value->vlu_misc.vlu_dec128 = *((Decimal128*) from.dsc_address);
		return;

	case dtype_int128:
		value->vlu_misc.vlu_int128 = *((Int128*) from.dsc_address);
		return;

	case dtype_sql_time:
		value->vlu_misc.vlu_sql_time = *(GDS_TIME*) from.dsc_address;
		return;

	case dtype_ex_time_tz:
	case dtype_sql_time_tz:
		value->vlu_misc.vlu_sql_time_tz = *(ISC_TIME_TZ*) from.dsc_address;
		return;

	case dtype_sql_date:
		value->vlu_misc.vlu_sql_date = *(GDS_DATE*) from.dsc_address;
		return;

	case dtype_timestamp:
		value->vlu_misc.vlu_timestamp = *(GDS_TIMESTAMP*) from.dsc_address;
		return;

	case dtype_ex_timestamp_tz:
	case dtype_timestamp_tz:
		value->vlu_misc.vlu_timestamp_tz = *(ISC_TIMESTAMP_TZ*) from.dsc_address;
		return;

	case dtype_quad:
		value->vlu_misc.vlu_dbkey[0] = ((SLONG*) from.dsc_address)[0];
		value->vlu_misc.vlu_dbkey[1] = ((SLONG*) from.dsc_address)[1];
		return;

	case dtype_text:
	case dtype_varying:
	case dtype_cstring:
	case dtype_dbkey:
		break;

	case dtype_blob:
		value->vlu_misc.vlu_bid = *(bid*) from.dsc_address;
		return;

	case dtype_boolean:
		value->vlu_misc.vlu_uchar = *from.dsc_address;
		return;

	default:
		fb_assert(false);
		break;
	}

	VaryStr<TEMP_STR_LENGTH> temp;
	UCHAR* address;
	USHORT ttype;

	// Get string.  If necessary, get_string will convert the string into a
	// temporary buffer.  Since this will always be the result of a conversion,
	// this isn't a serious problem.

	const USHORT length = MOV_get_string_ptr(tdbb, &from, &ttype, &address, &temp, sizeof(temp));

	// Allocate a string block of sufficient size.

	VaryingString* string = value->vlu_string;

	if (string && string->str_length < length)
	{
		delete string;
		string = NULL;
	}

	if (!string)
	{
		if (!pool)
			pool = tdbb->getDefaultPool();

		string = value->vlu_string = FB_NEW_RPT(*pool, length) VaryingString();
		string->str_length = length;
	}

	value->vlu_desc.dsc_length = length;
	UCHAR* target = string->str_data;
	value->vlu_desc.dsc_address = target;
	value->vlu_desc.dsc_sub_type = 0;
	value->vlu_desc.dsc_scale = 0;

	if (from.dsc_dtype == dtype_dbkey)
		value->vlu_desc.dsc_dtype = dtype_dbkey;
	else
	{
		value->vlu_desc.dsc_dtype = dtype_text;
		value->vlu_desc.setTextType(ttype);
	}

	if (address && length && target != address)
		memcpy(target, address, length);
}


void EVL_validate(thread_db* tdbb, const Item& item, const ItemInfo* itemInfo, dsc* desc, bool null)
{
/**************************************
 *
 *	E V L _ v a l i d a t e
 *
 **************************************
 *
 * Functional description
 *	Validate argument/variable for not null and check constraint
 *
 **************************************/
	if (itemInfo == NULL)
		return;

	Request* request = tdbb->getRequest();
	bool err = false;

	if (null && !itemInfo->nullable)
		err = true;

	const char* value = NULL_STRING_MARK;
	VaryStr<TEMP_STR_LENGTH> temp;

	MapFieldInfo::ValueType fieldInfo;
	if (!err && itemInfo->fullDomain &&
		request->getStatement()->mapFieldInfo.get(itemInfo->field, fieldInfo) &&
		fieldInfo.validationExpr)
	{
		if (desc && null)
			desc->dsc_flags |= DSC_null;

		const bool desc_is_null = !desc || (desc->dsc_flags & DSC_null);

		request->req_domain_validation = desc;
		const ULONG flags = request->req_flags;

		if (!fieldInfo.validationExpr->execute(tdbb, request) && !(request->req_flags & req_null))
		{
			const USHORT length = desc_is_null ? 0 :
				MOV_make_string(tdbb, desc, ttype_dynamic, &value, &temp, sizeof(temp) - 1);

			if (desc_is_null)
				value = NULL_STRING_MARK;
			else if (!length)
				value = "";
			else
				const_cast<char*>(value)[length] = 0;	// safe cast - data is on our local stack

			err = true;
		}

		request->req_flags = flags;
	}

	if (err)
	{
		ISC_STATUS status = isc_not_valid_for_var;
		string s;
		const char* arg;

		if (item.type == Item::TYPE_CAST)
		{
			status = isc_not_valid_for;
			arg = "CAST";
		}
		else
		{
			s = item.getDescription(request, itemInfo);
			arg = s.c_str();
		}

		ERR_post(Arg::Gds(status) << Arg::Str(arg) << Arg::Str(value));
	}
}
