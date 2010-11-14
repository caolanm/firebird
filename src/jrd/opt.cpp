/*
 *	PROGRAM:	JRD Access Method
 *	MODULE:		opt.cpp
 *	DESCRIPTION:	Optimizer / record selection expression compiler
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
 * 2002.10.12: Nickolay Samofatov: Fixed problems with wrong results produced by
 *            outer joins
 * 2001.07.28: John Bellardo: Added code to handle rse_skip nodes.
 * 2001.07.17 Claudio Valderrama: Stop crash with indices and recursive calls
 *            of OPT_compile: indicator csb_indices set to zero after used memory is
 *            returned to the free pool.
 * 2001.02.15: Claudio Valderrama: Don't obfuscate the plan output if a selectable
 *             stored procedure doesn't access tables, views or other procedures directly.
 * 2002.10.29 Sean Leyne - Removed obsolete "Netware" port
 * 2002.10.30: Arno Brinkman: Changes made to gen_retrieval, OPT_compile and make_inversion.
 *             Procedure sort_indices added. The changes in gen_retrieval are that now
 *             an index with high field-count has priority to build an index from.
 *             Procedure make_inversion is changed so that it not pick every index
 *             that comes away, this was slow performance with bad selectivity indices
 *             which most are foreign_keys with a reference to a few records.
 * 2002.11.01: Arno Brinkman: Added match_indices for better support of OR handling
 *             in INNER JOIN (gen_join) statements.
 * 2002.12.15: Arno Brinkman: Added find_used_streams, so that inside opt_compile all the
 *             streams are marked active. This causes that more indices can be used for
 *             a retrieval. With this change BUG SF #219525 is solved too.
 */

#include "firebird.h"
#include "../common/common.h"
#include <stdio.h>
#include <string.h>
#include "../jrd/ibase.h"
#include "../jrd/jrd.h"
#include "../jrd/align.h"
#include "../jrd/val.h"
#include "../jrd/req.h"
#include "../jrd/exe.h"
#include "../jrd/lls.h"
#include "../jrd/ods.h"
#include "../jrd/btr.h"
#include "../jrd/sort.h"
#include "../jrd/rse.h"
#include "../jrd/ini.h"
#include "../jrd/intl.h"
#include "../common/gdsassert.h"
#include "../jrd/btr_proto.h"
#include "../jrd/cch_proto.h"
#include "../jrd/cmp_proto.h"
#include "../jrd/dpm_proto.h"
#include "../common/dsc_proto.h"
#include "../jrd/err_proto.h"
#include "../jrd/ext_proto.h"
#include "../jrd/intl_proto.h"

#include "../jrd/lck_proto.h"
#include "../jrd/met_proto.h"
#include "../jrd/mov_proto.h"
#include "../jrd/opt_proto.h"
#include "../jrd/par_proto.h"
#include "../yvalve/gds_proto.h"
#include "../jrd/DataTypeUtil.h"
#include "../jrd/RecordSourceNodes.h"
#include "../jrd/VirtualTable.h"
#include "../jrd/DatabaseSnapshot.h"
#include "../jrd/UserManagement.h"
#include "../common/classes/array.h"
#include "../common/classes/objects_array.h"
#include "../jrd/recsrc/RecordSource.h"
#include "../jrd/recsrc/Cursor.h"

#include "../jrd/Optimizer.h"
#include "../dsql/BoolNodes.h"
#include "../dsql/ExprNodes.h"
#include "../dsql/StmtNodes.h"

using namespace Jrd;
using namespace Firebird;

#ifdef DEV_BUILD
#define OPT_DEBUG
#endif


bool JrdNodeVisitor::visitChildren(const JrdNode& node)
{
	bool ret = false;

	if (!node)
		return ret;

	if (node.boolExprNode)
		return call(*node.boolExprNode);

	jrd_nod* jrdNode = (*node.jrdNode);

	switch (jrdNode->nod_type)
	{
		case nod_class_exprnode_jrd:
		{
			ExprNode* exprNode = reinterpret_cast<ExprNode*>(jrdNode->nod_arg[0]);
			return call(exprNode);
		}

		default:
			return returnOnOthers;
	}

	return ret;
}


PossibleUnknownFinder::PossibleUnknownFinder()
	: JrdNodeVisitor(true, &ExprNode::jrdPossibleUnknownFinder)
{
}

bool PossibleUnknownFinder::visit(const JrdNode& node)
{
	DEV_BLKCHK(node, type_nod);

	if (!node)
		return false;

	if (node.boolExprNode)
		return visitChildren(node);

	jrd_nod* jrdNode = (*node.jrdNode);

	return visitChildren(node);
}


StreamFinder::StreamFinder(CompilerScratch* aCsb, UCHAR aStream)
	: JrdNodeVisitor(true, &ExprNode::jrdStreamFinder),
	  csb(aCsb),
	  stream(aStream)
{
}

bool StreamFinder::visit(const JrdNode& node)
{
	DEV_BLKCHK(node, type_nod);

	if (!node)
		return false;

	if (node.boolExprNode)
		return visitChildren(node);

	jrd_nod* jrdNode = (*node.jrdNode);

	switch (jrdNode->nod_type)
	{
		case nod_class_recsrcnode_jrd:
		{
			RecordSourceNode* recSource = reinterpret_cast<RecordSourceNode*>(jrdNode->nod_arg[0]);

			if (recSource->type == RseNode::TYPE)
			{
				RseNode* rse = static_cast<RseNode*>(recSource);

				if (rse->rse_first && visit(rse->rse_first))
					return true;

				if (rse->rse_skip && visit(rse->rse_skip))
					return true;

				if (rse->rse_boolean && rse->rse_boolean->jrdStreamFinder(*this))
					return true;

				// ASF: The legacy code used to visit rse_sorted and rse_projection. But note that
				// visiting them, the visitor always returns true, because nod_sort is not handled
				// there. So I replaced these lines by the if/return below.
				//
				// if (rse->rse_sorted && visit(rse->rse_sorted))
				//     return true;
				//
				// if (rse->rse_projection && visit(rse->rse_projection))
				//     return true;

				if (rse->rse_sorted || rse->rse_projection)
					return true;
			}
			else if (recSource->type == ProcedureSourceNode::TYPE)
				return visit(static_cast<ProcedureSourceNode*>(recSource)->inputs);
			else
				return visitChildren(node);

			break;
		}

		default:
			return visitChildren(node);
	}

	return false;
}


StreamsCollector::StreamsCollector(SortedArray<int>& aStreams)
	: JrdNodeVisitor(false, &ExprNode::jrdStreamsCollector),
	  streams(aStreams)
{
}

bool StreamsCollector::visit(const JrdNode& node)
{
	DEV_BLKCHK(node, type_nod);

	if (!node)
		return false;

	if (node.boolExprNode)
		return visitChildren(node);

	jrd_nod* jrdNode = (*node.jrdNode);

	switch (jrdNode->nod_type)
	{
		case nod_class_recsrcnode_jrd:
		{
			RecordSourceNode* recSource = reinterpret_cast<RecordSourceNode*>(jrdNode->nod_arg[0]);

			if (recSource->type == RseNode::TYPE)
			{
				RseNode* rse = static_cast<RseNode*>(recSource);
				visit(rse->rse_first);
				visit(rse->rse_skip);

				if (rse->rse_boolean)
					rse->rse_boolean->jrdStreamsCollector(*this);

				// ASF: The legacy code used to visit rse_sorted and rse_projection, but the visitor
				// never handled nod_sort.
				// visit(rse->rse_sorted);
				// visit(rse->rse_projection);
			}
			else if (recSource->type == ProcedureSourceNode::TYPE)
				return visit(static_cast<ProcedureSourceNode*>(recSource)->inputs);
			else
				return visitChildren(node);

			break;
		}

		default:
			return visitChildren(node);
	}

	return false;
}


UnmappedNodeGetter::UnmappedNodeGetter(/*const*/ MapNode* aMap, UCHAR aShellStream)
	: JrdNodeVisitor(false, &ExprNode::jrdUnmappedNodeGetter),
	  map(aMap),
	  shellStream(aShellStream),
	  invalid(false)
{
	DEV_BLKCHK(map, type_nod);
}

bool UnmappedNodeGetter::visit(const JrdNode& node)
{
	if (node.boolExprNode)
	{
		invalid |= !visitChildren(node);
		return !invalid;
	}

	invalid |= !visitChildren(node);

	return !invalid;
}


namespace Jrd
{
	class River
	{
	public:
		River(CompilerScratch* csb, RecordSource* rsb, RecordSourceNode* node, size_t count, UCHAR* streams)
			: m_rsb(rsb), m_node(node), m_streams(csb->csb_pool)
		{
			m_streams.resize(count);
			memcpy(m_streams.begin(), streams, count);
		}

		River(CompilerScratch* csb, RecordSource* rsb, RiverList& rivers)
			: m_rsb(rsb), m_node(NULL), m_streams(csb->csb_pool)
		{
			for (River** iter = rivers.begin(); iter < rivers.end(); iter++)
			{
				River* const sub_river = *iter;

				const size_t count = m_streams.getCount();
				const size_t delta = sub_river->m_streams.getCount();
				m_streams.grow(count + delta);
				memcpy(m_streams.begin() + count, sub_river->m_streams.begin(), delta);
			}
		}

		RecordSource* getRecordSource() const
		{
			return m_rsb;
		}

		size_t getStreamCount() const
		{
			return m_streams.getCount();
		}

		const UCHAR* getStreams() const
		{
			return m_streams.begin();
		}

		void activate(CompilerScratch* csb)
		{
			for (const UCHAR* iter = m_streams.begin(); iter < m_streams.end(); iter++)
			{
				csb->csb_rpt[*iter].csb_flags |= csb_active;
			}
		}

		void deactivate(CompilerScratch* csb)
		{
			for (const UCHAR* iter = m_streams.begin(); iter < m_streams.end(); iter++)
			{
				csb->csb_rpt[*iter].csb_flags &= ~csb_active;
			}
		}

		bool isReferenced(const jrd_nod* node) const
		{
			bool field_found = false;

			if (isReferenced(node, field_found))
				return field_found;

			return false;
		}

		bool isComputable(CompilerScratch* csb) const
		{
			return m_node ? m_node->computable(csb, -1, false, false, NULL) : true;
		}

	protected:
		bool isReferenced(const jrd_nod* node, bool& field_found) const
		{
			const FieldNode* fieldNode;

			if ((fieldNode = ExprNode::as<FieldNode>(node)))
			{
				for (const UCHAR* iter = m_streams.begin(); iter != m_streams.end(); ++iter)
				{
					if (fieldNode->fieldStream == *iter)
					{
						field_found = true;
						return true;
					}
				}

				return false;
			}

			const jrd_nod* const* ptr = node->nod_arg;

			for (const jrd_nod* const* const end = ptr + node->nod_count; ptr != end; ++ptr)
			{
				if (!isReferenced(*ptr, field_found))
					return false;
			}

			return true;
		}

		RecordSource* m_rsb;
		RecordSourceNode* const m_node;
		StreamList m_streams;
	};

	class CrossJoin : public River
	{
	public:
		CrossJoin(CompilerScratch* csb, RiverList& rivers)
			: River(csb, NULL, rivers)
		{
			const size_t count = rivers.getCount();
			fb_assert(count);

			if (count == 1)
			{
				m_rsb = rivers.front()->getRecordSource();
			}
			else
			{
				HalfStaticArray<RecordSource*, OPT_STATIC_ITEMS> rsbs;

				// Reorder input rivers according to their possible inter-dependencies

				while (rsbs.getCount() < count)
				{
					for (River** iter = rivers.begin(); iter < rivers.end(); iter++)
					{
						River* const sub_river = *iter;
						RecordSource* const sub_rsb = sub_river->getRecordSource();

						if (!rsbs.exist(sub_rsb) && sub_river->isComputable(csb))
						{
							rsbs.add(sub_rsb);
							sub_river->activate(csb);
						}
					}
				}

				m_rsb = FB_NEW(csb->csb_pool) NestedLoopJoin(csb, count, rsbs.begin());
			}

			rivers.clear();
		}
	};
} // namespace

static bool augment_stack(jrd_nod*, NodeStack&);
static bool augment_stack(BoolExprNode*, BoolExprNodeStack&);
static void check_indices(const CompilerScratch::csb_repeat*);
static void check_sorts(RseNode*);
static void class_mask(USHORT, jrd_nod**, ULONG *);
static BoolExprNode* compose(thread_db*, BoolExprNode**, BoolExprNode*);
static bool check_for_nod_from(const jrd_nod*);
static SLONG decompose(thread_db*, BoolExprNode*, BoolExprNodeStack&, CompilerScratch*);
static USHORT distribute_equalities(BoolExprNodeStack&, CompilerScratch*, USHORT);
static void find_index_relationship_streams(thread_db*, OptimizerBlk*, const UCHAR*, UCHAR*, UCHAR*);
static void form_rivers(thread_db*, OptimizerBlk*, const UCHAR*, RiverList&, SortNode**, PlanNode*);
static bool form_river(thread_db*, OptimizerBlk*, USHORT, USHORT, UCHAR*, RiverList&, SortNode**);
static void gen_join(thread_db*, OptimizerBlk*, const UCHAR*, RiverList&, SortNode**, PlanNode*);
static RecordSource* gen_outer(thread_db*, OptimizerBlk*, RseNode*, RiverList&, SortNode**);
static RecordSource* gen_residual_boolean(thread_db*, OptimizerBlk*, RecordSource*);
static RecordSource* gen_retrieval(thread_db*, OptimizerBlk*, SSHORT, SortNode**, bool, bool, BoolExprNode**);
static bool gen_equi_join(thread_db*, OptimizerBlk*, RiverList&);
static BoolExprNode* make_inference_node(CompilerScratch*, BoolExprNode*, jrd_nod*, jrd_nod*);
static bool map_equal(const jrd_nod*, const jrd_nod*, const MapNode*);
static bool node_equality(const jrd_nod*, const jrd_nod*);
static bool node_equality(const BoolExprNode*, const BoolExprNode*);
static jrd_nod* optimize_like(thread_db*, CompilerScratch*, ComparativeBoolNode*);
static USHORT river_count(USHORT, jrd_nod**);
static bool search_stack(const jrd_nod*, const NodeStack&);
static void set_direction(SortNode*, SortNode*);
static void set_position(const SortNode*, SortNode*, const MapNode*);
static void set_rse_inactive(CompilerScratch*, const RseNode*);


// macro definitions

#ifdef OPT_DEBUG
const int DEBUG_PUNT			= 5;
const int DEBUG_RELATIONSHIPS	= 4;
const int DEBUG_ALL				= 3;
const int DEBUG_CANDIDATE		= 2;
const int DEBUG_BEST			= 1;
const int DEBUG_NONE			= 0;

FILE *opt_debug_file = 0;
static int opt_debug_flag = DEBUG_NONE;
#endif

inline void SET_DEP_BIT(ULONG* array, const SLONG bit)
{
	array[bit / 32] |= (1L << (bit % 32));
}

inline void CLEAR_DEP_BIT(ULONG* array, const SLONG bit)
{
	array[bit / 32] &= ~(1L << (bit % 32));
}

inline bool TEST_DEP_BIT(const ULONG* array, const ULONG bit)
{
	return (array[bit / 32] & (1L << (bit % 32))) != 0;
}

inline bool TEST_DEP_ARRAYS(const ULONG* ar1, const ULONG* ar2)
{	return (ar1[0] & ar2[0]) || (ar1[1] & ar2[1]) || (ar1[2] & ar2[2]) || (ar1[3] & ar2[3]) ||
		   (ar1[4] & ar2[4]) || (ar1[5] & ar2[5]) || (ar1[6] & ar2[6]) || (ar1[7] & ar2[7]);
}

// some arbitrary fudge factors for calculating costs, etc.--
// these could probably be better tuned someday

const double ESTIMATED_SELECTIVITY			= 0.01;
const int INVERSE_ESTIMATE					= 10;
const double INDEX_COST						= 30.0;
const int CACHE_PAGES_PER_STREAM			= 15;
const int SELECTIVITY_THRESHOLD_FACTOR		= 10;
const int OR_SELECTIVITY_THRESHOLD_FACTOR	= 2000;
const FB_UINT64 LOWEST_PRIORITY_LEVEL		= 0;

// enumeration of sort datatypes

static const UCHAR sort_dtypes[] =
{
	0,							// dtype_unknown
	SKD_text,					// dtype_text
	SKD_cstring,				// dtype_cstring
	SKD_varying,				// dtype_varying
	0,
	0,
	0,							// dtype_packed
	0,							// dtype_byte
	SKD_short,					// dtype_short
	SKD_long,					// dtype_long
	SKD_quad,					// dtype_quad
	SKD_float,					// dtype_real
	SKD_double,					// dtype_double
	SKD_double,					// dtype_d_float
	SKD_sql_date,				// dtype_sql_date
	SKD_sql_time,				// dtype_sql_time
	SKD_timestamp,				// dtype_timestamp
	SKD_quad,					// dtype_blob
	0,							// dtype_array
	SKD_int64,					// dtype_int64
	SKD_text					// dtype_dbkey - use text sort for backward compatibility
};


bool OPT_access_path(const jrd_req* request, UCHAR* buffer, SLONG buffer_length, ULONG* return_length)
{
/**************************************
 *
 *	O P T _ a c c e s s _ p a t h
 *
 **************************************
 *
 * Functional description
 *	Returns a formatted access path for all
 *	RseNode's in the specified request.
 *
 **************************************/
	DEV_BLKCHK(request, type_req);

	thread_db* tdbb = JRD_get_thread_data();

	if (!buffer || buffer_length < 0 || !return_length)
		return false;

	// loop through all RSEs in the request, and describe the rsb tree for that rsb

	UCharBuffer infoBuffer;

	Array<const RecordSource*>& fors = request->getStatement()->fors;
	for (size_t i = 0; i < fors.getCount(); i++)
		fors[i]->dump(tdbb, infoBuffer);

	const size_t length = infoBuffer.getCount();

	if (length > static_cast<ULONG>(buffer_length))
	{
		*return_length = 0;
		return false;
	}

	*return_length = (ULONG) length;
	memcpy(buffer, infoBuffer.begin(), length);
	return true;
}


// Compile and optimize a record selection expression into a set of record source blocks (rsb's).
RecordSource* OPT_compile(thread_db* tdbb, CompilerScratch* csb, RseNode* rse,
	BoolExprNodeStack* const parent_stack)
{
	DEV_BLKCHK(csb, type_csb);
	DEV_BLKCHK(rse, type_nod);

	SET_TDBB(tdbb);

#ifdef OPT_DEBUG
	if (opt_debug_flag != DEBUG_NONE && !opt_debug_file)
		opt_debug_file = fopen("opt_debug.out", "w");
#endif

	// If there is a boolean, there is some work to be done.  First,
	// decompose the boolean into conjunctions.  Then get descriptions
	// of all indices for all relations in the RseNode.  This will give
	// us the info necessary to allocate a optimizer block big
	// enough to hold this crud.

	// Do not allocate the index_desc struct. Let BTR_all do the job. The allocated
	// memory will then be in csb->csb_rpt[stream].csb_idx_allocation, which
	// gets cleaned up before this function exits.

	AutoPtr<OptimizerBlk> opt(FB_NEW(*tdbb->getDefaultPool()) OptimizerBlk(tdbb->getDefaultPool(),
		rse, parent_stack));
	opt->opt_streams.grow(csb->csb_n_stream);
	RecordSource* rsb = NULL;

	try {

	opt->opt_csb = csb;

	RiverList rivers;

	check_sorts(rse);
	SortNode* sort = rse->rse_sorted;
	SortNode* project = rse->rse_projection;
	SortNode* aggregate = rse->rse_aggregate;

	// put any additional booleans on the conjunct stack, and see if we
	// can generate additional booleans by associativity--this will help
	// to utilize indices that we might not have noticed
	if (rse->rse_boolean)
		opt->conjunctCount = decompose(tdbb, rse->rse_boolean, opt->conjunctStack, csb);

	opt->conjunctCount += distribute_equalities(opt->conjunctStack, csb, opt->conjunctCount);

	// AB: If we have limit our retrieval with FIRST / SKIP syntax then
	// we may not deliver above conditions (from higher rse's) to this
	// rse, because the results should be consistent.
    if (rse->rse_skip || rse->rse_first)
		opt->parentStack = NULL;

	// clear the csb_active flag of all streams in the RseNode
	set_rse_inactive(csb, rse);

	// go through the record selection expression generating
	// record source blocks for all streams

	NestConst<RecordSourceNode>* ptr = rse->rse_relations.begin();
	for (NestConst<RecordSourceNode>* const end = rse->rse_relations.end(); ptr != end; ++ptr)
	{
		RecordSourceNode* node = *ptr;

		opt->localStreams[0] = 0;

		fb_assert(sort == rse->rse_sorted);
		fb_assert(aggregate == rse->rse_aggregate);

		// find the stream number and place it at the end of the beds array
		// (if this is really a stream and not another RseNode)

		rsb = node->compile(tdbb, opt, (ptr - rse->rse_relations.begin() == 1));

		// if an rsb has been generated, we have a non-relation;
		// so it forms a river of its own since it is separately
		// optimized from the streams in this rsb

		if (rsb)
		{
			// AB: Save all inner-part streams
			if (rse->rse_jointype == blr_inner ||
			   (rse->rse_jointype == blr_left && (ptr - rse->rse_relations.begin()) == 0))
			{
				rsb->findUsedStreams(opt->subStreams);
				// Save also the outer streams
				if (rse->rse_jointype == blr_left)
					rsb->findUsedStreams(opt->outerStreams);
			}

			const size_t count = opt->localStreams[0];
			UCHAR* const streams = opt->localStreams + 1;
			River* const river = FB_NEW(*tdbb->getDefaultPool()) River(csb, rsb, node, count, streams);
			river->deactivate(csb);
			rivers.add(river);
		}
	}

	// this is an attempt to make sure we have a large enough cache to
	// efficiently retrieve this query; make sure the cache has a minimum
	// number of pages for each stream in the RseNode (the number is just a guess)
	if (opt->compileStreams[0] > 5)
		CCH_expand(tdbb, (ULONG) (opt->compileStreams[0] * CACHE_PAGES_PER_STREAM));

	// At this point we are ready to start optimizing.
	// We will use the opt block to hold information of
	// a global nature, meaning that it needs to stick
	// around for the rest of the optimization process.

	// Set base-point before the parent/distributed nodes begin.
	const USHORT base_count = (USHORT) opt->conjunctCount;
	opt->opt_base_conjuncts = base_count;

	// AB: Add parent conjunctions to opt->conjunctStack, keep in mind
	// the outer-streams! For outer streams put missing (IS NULL)
	// conjunctions in the missing_stack.
	//
	// opt_rpt[0..opt_base_conjuncts-1] = defined conjunctions to this stream
	// opt_rpt[0..opt_base_parent_conjuncts-1] = defined conjunctions to this
	//   stream and allowed distributed conjunctions (with parent)
	// opt_rpt[0..opt_base_missing_conjuncts-1] = defined conjunctions to this
	//   stream and allowed distributed conjunctions and allowed parent
	// opt_rpt[0..opt_conjuncts_count-1] = all conjunctions
	//
	// allowed = booleans that can never evaluate to NULL/Unknown or turn
	//   NULL/Unknown into a True or False.

	USHORT parent_count = 0, distributed_count = 0;
	BoolExprNodeStack missing_stack;

	if (opt->parentStack)
	{
		for (BoolExprNodeStack::iterator iter(*opt->parentStack);
			 iter.hasData() && opt->conjunctCount < MAX_CONJUNCTS; ++iter)
		{
			BoolExprNode* const node = iter.object();

			PossibleUnknownFinder finder;

			if (rse->rse_jointype != blr_inner && node->jrdPossibleUnknownFinder(finder))
			{
				// parent missing conjunctions shouldn't be
				// distributed to FULL OUTER JOIN streams at all
				if (rse->rse_jointype != blr_full)
					missing_stack.push(node);
			}
			else
			{
				opt->conjunctStack.push(node);
				opt->conjunctCount++;
				parent_count++;
			}
		}

		// We've now merged parent, try again to make more conjunctions.
		distributed_count = distribute_equalities(opt->conjunctStack, csb, opt->conjunctCount);
		opt->conjunctCount += distributed_count;
	}

	// The newly created conjunctions belong to the base conjunctions.
	// After them are starting the parent conjunctions.
	opt->opt_base_parent_conjuncts = opt->opt_base_conjuncts + distributed_count;

	// Set base-point before the parent IS NULL nodes begin
	opt->opt_base_missing_conjuncts = (USHORT) opt->conjunctCount;

	// Check if size of optimizer block exceeded.
	if (opt->conjunctCount > MAX_CONJUNCTS)
	{
		ERR_post(Arg::Gds(isc_optimizer_blk_exc));
		// Msg442: size of optimizer block exceeded
	}

	// Put conjunctions in opt structure.
	// Note that it's a stack and we get the nodes in reversed order from the stack.

	opt->opt_conjuncts.grow(opt->conjunctCount);
	SSHORT nodeBase = -1, j = -1;

	for (SLONG i = opt->conjunctCount; i > 0; i--, j--)
	{
		BoolExprNode* const node = opt->conjunctStack.pop();

		if (i == base_count)
		{
			// The base conjunctions
			j = base_count - 1;
			nodeBase = 0;
		}
		else if (i == opt->conjunctCount - distributed_count)
		{
			// The parent conjunctions
			j = parent_count - 1;
			nodeBase = opt->opt_base_parent_conjuncts;
		}
		else if (i == opt->conjunctCount)
		{
			// The new conjunctions created by "distribution" from the stack
			j = distributed_count - 1;
			nodeBase = opt->opt_base_conjuncts;
		}

		fb_assert(nodeBase >= 0 && j >= 0);
		opt->opt_conjuncts[nodeBase + j].opt_conjunct_node = node;
	}

	// Put the parent missing nodes on the stack
	for (BoolExprNodeStack::iterator iter(missing_stack);
		 iter.hasData() && opt->conjunctCount < MAX_CONJUNCTS; ++iter)
	{
		BoolExprNode* const node = iter.object();

		opt->opt_conjuncts.grow(opt->conjunctCount + 1);
		opt->opt_conjuncts[opt->conjunctCount].opt_conjunct_node = node;
		opt->conjunctCount++;
	}

	// Deoptimize some conjuncts in advance
	for (size_t iter = 0; iter < opt->opt_conjuncts.getCount(); iter++)
	{
		if (opt->opt_conjuncts[iter].opt_conjunct_node->flags & BoolExprNode::FLAG_DEOPTIMIZE)
		{
			// Fake an index match for them
			opt->opt_conjuncts[iter].opt_conjunct_flags |= opt_conjunct_matched;
		}
	}

	// attempt to optimize aggregates via an index, if possible
	if (aggregate && !sort)
		sort = aggregate;
	else
		rse->rse_aggregate = aggregate = NULL;

	// AB: Mark the previous used streams (sub-RseNode's) as active
	for (StreamsArray::iterator i = opt->subStreams.begin(); i != opt->subStreams.end(); ++i)
		csb->csb_rpt[*i].csb_flags |= csb_active;

	// outer joins require some extra processing
	if (rse->rse_jointype != blr_inner)
		rsb = gen_outer(tdbb, opt, rse, rivers, &sort);
	else
	{
		bool sort_can_be_used = true;
		SortNode* const saved_sort_node = sort;

		// AB: If previous rsb's are already on the stack we can't use
		// a navigational-retrieval for an ORDER BY because the next
		// streams are JOINed to the previous ones
		if (rivers.hasData())
		{
			sort = NULL;
			sort_can_be_used = false;
			// AB: We could already have multiple rivers at this
			// point so try to do some hashing or sort/merging now.
			while (gen_equi_join(tdbb, opt, rivers))
				;

			// AB: Mark the previous used streams (sub-RseNode's) again
			// as active, because a SORT/MERGE could reset the flags
			for (StreamsArray::iterator i = opt->subStreams.begin(); i != opt->subStreams.end(); ++i)
				csb->csb_rpt[*i].csb_flags |= csb_active;
		}

		fb_assert(opt->compileStreams[0] != 1 || csb->csb_rpt[opt->compileStreams[1]].csb_relation != 0);

		while (true)
		{
			// AB: Determine which streams have an index relationship
			// with the currently active rivers. This is needed so that
			// no merge is made between a new cross river and the
			// currently active rivers. Where in the new cross river
			// a stream depends (index) on the active rivers.
			stream_array_t dependent_streams, free_streams;
			dependent_streams[0] = free_streams[0] = 0;
			find_index_relationship_streams(tdbb, opt, opt->compileStreams, dependent_streams, free_streams);

			// If we have dependent and free streams then we can't rely on
			// the sort node to be used for index navigation.
			if (dependent_streams[0] && free_streams[0])
			{
				sort = NULL;
				sort_can_be_used = false;
			}

			if (dependent_streams[0])
			{
				// copy free streams
				for (USHORT i = 0; i <= free_streams[0]; i++) {
					opt->compileStreams[i] = free_streams[i];
				}

				// Make rivers from the dependent streams
				gen_join(tdbb, opt, dependent_streams, rivers, &sort, rse->rse_plan);

				// Generate one river which holds a cross join rsb between
				// all currently available rivers

				River* const river = FB_NEW(*tdbb->getDefaultPool()) CrossJoin(csb, rivers);
				rivers.add(river);
			}
			else
			{
				if (free_streams[0])
				{
					// Deactivate streams from rivers on stack, because
					// the remaining streams don't have any indexed relationship with them
					for (River** iter = rivers.begin(); iter < rivers.end(); iter++)
					{
						(*iter)->deactivate(csb);
					}
				}

				break;
			}
		}

		// attempt to form joins in decreasing order of desirability
		gen_join(tdbb, opt, opt->compileStreams, rivers, &sort, rse->rse_plan);

		// If there are multiple rivers, try some hashing or sort/merging
		while (gen_equi_join(tdbb, opt, rivers))
			;

		rsb = CrossJoin(csb, rivers).getRecordSource();

		// Assign the sort node back if it wasn't used by the index navigation
		if (saved_sort_node && !sort_can_be_used)
		{
			sort = saved_sort_node;
		}

		// Pick up any residual boolean that may have fallen thru the cracks
		rsb = gen_residual_boolean(tdbb, opt, rsb);
	}

	// if the aggregate was not optimized via an index, get rid of the
	// sort and flag the fact to the calling routine
	if (aggregate && sort)
	{
		rse->rse_aggregate = NULL;
		sort = NULL;
	}

	// check index usage in all the base streams to ensure
	// that any user-specified access plan is followed

	for (USHORT i = 1; i <= opt->compileStreams[0]; i++)
		check_indices(&csb->csb_rpt[opt->compileStreams[i]]);

	if (project || sort)
	{
		// Eliminate any duplicate dbkey streams
		const UCHAR* const b_end = opt->beds + opt->beds[0];
		const UCHAR* const k_end = opt->keyStreams + opt->keyStreams[0];
		UCHAR* k = &opt->keyStreams[1];
		for (const UCHAR* p2 = k; p2 <= k_end; p2++)
		{
			const UCHAR* q = &opt->beds[1];
			while (q <= b_end && *q != *p2) {
				q++;
			}
			if (q > b_end) {
				*k++ = *p2;
			}
		}
		opt->keyStreams[0] = k - &opt->keyStreams[1];

		// Handle project clause, if present
		if (project)
			rsb = OPT_gen_sort(tdbb, opt->opt_csb, opt->beds, opt->keyStreams, rsb, project, true);

		// Handle sort clause if present
		if (sort)
			rsb = OPT_gen_sort(tdbb, opt->opt_csb, opt->beds, opt->keyStreams, rsb, sort, false);
	}

    // Handle first and/or skip.  The skip MUST (if present)
    // appear in the rsb list AFTER the first.  Since the gen_first and gen_skip
    // functions add their nodes at the beginning of the rsb list we MUST call
    // gen_skip before gen_first.

    if (rse->rse_skip) {
		rsb = FB_NEW(*tdbb->getDefaultPool()) SkipRowsStream(csb, rsb, rse->rse_skip);
	}

	if (rse->rse_first) {
		rsb = FB_NEW(*tdbb->getDefaultPool()) FirstRowsStream(csb, rsb, rse->rse_first);
	}

	// release memory allocated for index descriptions
	for (USHORT i = 1; i <= opt->compileStreams[0]; ++i)
	{
		const USHORT loopStream = opt->compileStreams[i];
		delete csb->csb_rpt[loopStream].csb_idx;
		csb->csb_rpt[loopStream].csb_idx = NULL;

		// CVC: The following line added because OPT_compile is recursive, both directly
		//   and through gen_union(), too. Otherwise, we happen to step on deallocated memory
		//   and this is the cause of the crashes with indices that have plagued IB since v4.

		csb->csb_rpt[loopStream].csb_indices = 0;
	}

#ifdef OPT_DEBUG
	if (opt_debug_file)
	{
		fflush(opt_debug_file);
		//fclose(opt_debug_file);
		//opt_debug_file = 0;
	}
#endif

	}	// try
	catch (const Exception&)
	{
		for (USHORT i = 1; i <= opt->compileStreams[0]; ++i)
		{
			const USHORT loopStream = opt->compileStreams[i];
			delete csb->csb_rpt[loopStream].csb_idx;
			csb->csb_rpt[loopStream].csb_idx = NULL;
			csb->csb_rpt[loopStream].csb_indices = 0; // Probably needed to be safe
		}

		throw;
	}

	if (rse->flags & RseNode::FLAG_WRITELOCK)
	{
		for (USHORT i = 1; i <= opt->compileStreams[0]; ++i)
		{
			const USHORT loopStream = opt->compileStreams[i];
			csb->csb_rpt[loopStream].csb_flags |= csb_update;
		}
	}

	return rsb;
}


// Add node (jrd_nod) to stack unless node is already on stack.
static bool augment_stack(jrd_nod* node, NodeStack& stack)
{
/**************************************
 *
 *	a u g m e n t _ s t a c k
 *
 **************************************
 *
 * Functional description
 *
 **************************************/

	for (NodeStack::const_iterator temp(stack); temp.hasData(); ++temp)
	{
		if (node_equality(node, temp.object()))
			return false;
	}

	stack.push(node);

	return true;
}

// Add node (BoolExprNode) to stack unless node is already on stack.
static bool augment_stack(BoolExprNode* node, BoolExprNodeStack& stack)
{
	for (BoolExprNodeStack::const_iterator temp(stack); temp.hasData(); ++temp)
	{
		if (node_equality(node, temp.object()))
			return false;
	}

	stack.push(node);

	return true;
}


static void check_indices(const CompilerScratch::csb_repeat* csb_tail)
{
/**************************************
 *
 *	c h e c k _ i n d i c e s
 *
 **************************************
 *
 * Functional description
 *	Check to make sure that the user-specified
 *	indices were actually utilized by the optimizer.
 *
 **************************************/
	thread_db* tdbb = JRD_get_thread_data();

	const PlanNode* plan = csb_tail->csb_plan;
	if (!plan)
		return;

	if (plan->type != PlanNode::TYPE_RETRIEVE)
		return;

	const jrd_rel* relation = csb_tail->csb_relation;

	// if there were no indices fetched at all but the
	// user specified some, error out using the first index specified

	if (!csb_tail->csb_indices && plan->accessType)
	{
		// index %s cannot be used in the specified plan
		ERR_post(Arg::Gds(isc_index_unused) << plan->accessType->items[0].indexName);
	}

	// check to make sure that all indices are either used or marked not to be used,
	// and that there are no unused navigational indices
	MetaName index_name;

	const index_desc* idx = csb_tail->csb_idx->items;

	for (USHORT i = 0; i < csb_tail->csb_indices; i++)
	{
		if (!(idx->idx_runtime_flags & (idx_plan_dont_use | idx_used)) ||
			((idx->idx_runtime_flags & idx_plan_navigate) && !(idx->idx_runtime_flags & idx_navigate)))
		{
			if (relation)
				MET_lookup_index(tdbb, index_name, relation->rel_name, (USHORT) (idx->idx_id + 1));
			else
				index_name = "";

			// index %s cannot be used in the specified plan
			ERR_post(Arg::Gds(isc_index_unused) << Arg::Str(index_name));
		}

		++idx;
	}
}


static void check_sorts(RseNode* rse)
{
/**************************************
 *
 *	c h e c k _ s o r t s
 *
 **************************************
 *
 * Functional description
 *	Try to optimize out unnecessary sorting.
 *
 **************************************/
	DEV_BLKCHK(rse, type_nod);

	SortNode* sort = rse->rse_sorted;
	SortNode* project = rse->rse_projection;

	// check if a GROUP BY exists using the same fields as the project or sort:
	// if so, the projection can be eliminated; if no projection exists, then
	// the sort can be eliminated.

	SortNode* group;
	RecordSourceNode* sub_rse;

	if ((project || sort) && rse->rse_relations.getCount() == 1 && (sub_rse = rse->rse_relations[0]) &&
		sub_rse->type == AggregateSourceNode::TYPE &&
		(group = static_cast<AggregateSourceNode*>(sub_rse)->group))
	{
		// if all the fields of the project are the same as all the fields
		// of the group by, get rid of the project.

		if (project && (project->expressions.getCount() == group->expressions.getCount()))
		{
			NestConst<jrd_nod>* project_ptr = project->expressions.begin();
			const NestConst<jrd_nod>* const project_end = project->expressions.end();

			for (; project_ptr != project_end; ++project_ptr)
			{
				const NestConst<jrd_nod>* group_ptr = group->expressions.begin();
				const NestConst<jrd_nod>* const group_end = group->expressions.end();

				for (; group_ptr != group_end; ++group_ptr)
				{
					if (map_equal(*group_ptr, *project_ptr, static_cast<AggregateSourceNode*>(sub_rse)->map))
						break;
				}

				if (group_ptr == group_end)
					break;
			}

			// we can now ignore the project, but in case the project is being done
			// in descending order because of an order by, do the group by the same way.
			if (project_ptr == project_end)
			{
				set_direction(project, group);
				project = rse->rse_projection = NULL;
			}
		}

		// if there is no projection, then we can make a similar optimization
		// for sort, except that sort may have fewer fields than group by.
		if (!project && sort && (sort->expressions.getCount() <= group->expressions.getCount()))
		{
			const NestConst<jrd_nod>* sort_ptr = sort->expressions.begin();
			const NestConst<jrd_nod>* const sort_end = sort->expressions.end();

			for (; sort_ptr != sort_end; ++sort_ptr)
			{
				const NestConst<jrd_nod>* group_ptr = group->expressions.begin();
				const NestConst<jrd_nod>* const group_end = group->expressions.end();

				for (; group_ptr != group_end; ++group_ptr)
				{
					if (map_equal(*group_ptr, *sort_ptr, static_cast<AggregateSourceNode*>(sub_rse)->map))
						break;
				}

				if (group_ptr == group_end)
					break;
			}

			// if all the fields in the sort list match the first n fields in the
			// project list, we can ignore the sort, but update the sort order
			// (ascending/descending) to match that in the sort list

			if (sort_ptr == sort_end)
			{
				set_direction(sort, group);
				set_position(sort, group, static_cast<AggregateSourceNode*>(sub_rse)->map);
				sort = rse->rse_sorted = NULL;
			}
		}

	}

	// examine the ORDER BY and DISTINCT clauses; if all the fields in the
	// ORDER BY match the first n fields in the DISTINCT in any order, the
	// ORDER BY can be removed, changing the fields in the DISTINCT to match
	// the ordering of fields in the ORDER BY.

	if (sort && project && (sort->expressions.getCount() <= project->expressions.getCount()))
	{
		const NestConst<jrd_nod>* sort_ptr = sort->expressions.begin();
		const NestConst<jrd_nod>* const sort_end = sort->expressions.end();

		for (; sort_ptr != sort_end; ++sort_ptr)
		{
			const NestConst<jrd_nod>* project_ptr = project->expressions.begin();
			const NestConst<jrd_nod>* const project_end = project->expressions.end();

			for (; project_ptr != project_end; ++project_ptr)
			{
				const FieldNode* sortField = ExprNode::as<FieldNode>(sort_ptr->getObject());
				const FieldNode* projectField = ExprNode::as<FieldNode>(project_ptr->getObject());

				if (sortField && projectField &&
					sortField->fieldStream == projectField->fieldStream &&
					sortField->fieldId == projectField->fieldId)
				{
					break;
				}
			}

			if (project_ptr == project_end)
				break;
		}

		// if all the fields in the sort list match the first n fields
		// in the project list, we can ignore the sort, but update
		// the project to match the sort.
		if (sort_ptr == sort_end)
		{
			set_direction(sort, project);
			set_position(sort, project, NULL);
			sort = rse->rse_sorted = NULL;
		}
	}

	// RP: optimize sort with OUTER JOIN
	// if all the fields in the sort list are from one stream, check the stream is
	// the most outer stream, if true update rse and ignore the sort
	if (sort && !project)
	{
		int sort_stream = 0;
		bool usableSort = true;
		NestConst<jrd_nod>* sort_ptr = sort->expressions.begin();
		const NestConst<jrd_nod>* const sort_end = sort->expressions.end();

		for (; sort_ptr != sort_end; ++sort_ptr)
		{
			const FieldNode* sortField;

			if ((sortField = ExprNode::as<FieldNode>(sort_ptr->getObject())))
			{
				// Get stream for this field at this position.
				const int current_stream = sortField->fieldStream;

				// If this is the first position node, save this stream.
				if (sort_ptr == sort->expressions.begin())
			    	sort_stream = current_stream;
				else if (current_stream != sort_stream)
				{
					// If the current stream is different then the previous stream
					// then we can't use this sort for an indexed order retrieval.
					usableSort = false;
					break;
				}
			}
			else
			{
				// If this is not the first position node, reject this sort.
				// Two expressions cannot be mapped to a single index.
				if (sort_ptr > sort->expressions.begin())
				{
					usableSort = false;
					break;
				}

				// This position doesn't use a simple field, thus we should
				// check the expression internals.
				SortedArray<int> streams;
				StreamsCollector::collect(*sort_ptr, streams);

				// We can use this sort only if there's a single stream
				// referenced by the expression.
				if (streams.getCount() == 1)
					sort_stream = streams[0];
				else
				{
					usableSort = false;
					break;
				}
			}
		}

		if (usableSort)
		{
			RseNode* new_rse = NULL;
			RecordSourceNode* node = rse;

			while (node)
			{
				if (node->type == RseNode::TYPE)
				{
					new_rse = static_cast<RseNode*>(node);

					// AB: Don't distribute the sort when a FIRST/SKIP is supplied,
					// because that will affect the behaviour from the deeper RSE.
					// dimitr: the same rule applies to explicit/implicit user-defined sorts.
					if (new_rse != rse &&
						(new_rse->rse_first || new_rse->rse_skip ||
						 new_rse->rse_sorted || new_rse->rse_projection))
					{
						node = NULL;
						break;
					}

					// Walk trough the relations of the RSE and see if a
					// matching stream can be found.
					if (new_rse->rse_jointype == blr_inner)
					{
						if (new_rse->rse_relations.getCount() == 1)
							node = new_rse->rse_relations[0];
						else
						{
							bool sortStreamFound = false;
							for (size_t i = 0; i < new_rse->rse_relations.getCount(); i++)
							{
								RecordSourceNode* subNode = new_rse->rse_relations[i];

								if (subNode->type == RelationSourceNode::TYPE &&
									subNode->getStream() == sort_stream &&
									new_rse != rse)
								{
									// We have found the correct stream
									sortStreamFound = true;
									break;
								}

							}

							if (sortStreamFound)
							{
								// Set the sort to the found stream and clear the original sort
								new_rse->rse_sorted = sort;
								sort = rse->rse_sorted = NULL;
							}

							node = NULL;
						}
					}
					else if (new_rse->rse_jointype == blr_left)
						node = new_rse->rse_relations[0];
					else
						node = NULL;
				}
				else
				{
					if (node->type == RelationSourceNode::TYPE &&
						node->getStream() == sort_stream &&
						new_rse && new_rse != rse)
					{
						// We have found the correct stream, thus apply the sort here
						new_rse->rse_sorted = sort;
						sort = rse->rse_sorted = NULL;
					}

					node = NULL;
				}
			}
		}
	}
}


static void class_mask(USHORT count, jrd_nod** eq_class, ULONG* mask)
{
/**************************************
 *
 *	c l a s s _ m a s k
 *
 **************************************
 *
 * Functional description
 *	Given an sort/merge join equivalence class (vector of node pointers
 *	of representative values for rivers), return a bit mask of rivers
 *	with values.
 *
 **************************************/
	if (*eq_class) {
		DEV_BLKCHK(*eq_class, type_nod);
	}

	if (count > MAX_CONJUNCTS)
	{
		ERR_post(Arg::Gds(isc_optimizer_blk_exc));
		// Msg442: size of optimizer block exceeded
	}

	for (SLONG i = 0; i < OPT_STREAM_BITS; i++) {
		mask[i] = 0;
	}

	for (SLONG i = 0; i < count; i++, eq_class++)
	{
		if (*eq_class)
		{
			SET_DEP_BIT(mask, i);
			DEV_BLKCHK(*eq_class, type_nod);
		}
	}
}


static BoolExprNode* compose(thread_db* tdbb, BoolExprNode** node1, BoolExprNode* node2)
{
/**************************************
 *
 *	c o m p o s e
 *
 **************************************
 *
 * Functional description
 *	Build and AND out of two conjuncts.
 *
 **************************************/

	if (!node2)
		return *node1;

	if (!*node1)
		return (*node1 = node2);

	BinaryBoolNode* booleanNode = FB_NEW(*tdbb->getDefaultPool()) BinaryBoolNode(
		*tdbb->getDefaultPool(), blr_and);
	booleanNode->arg1 = *node1;
	booleanNode->arg2 = node2;

	return (*node1 = booleanNode);
}


static bool check_for_nod_from(const jrd_nod* node)
{
/**************************************
 *
 *	c h e c k _ f o r _ n o d _ f r o m
 *
 **************************************
 *
 * Functional description
 *	Check for nod_from under >=0 CastNode nodes.
 *
 **************************************/
	const CastNode* castNode;
	const SubQueryNode* subQueryNode;

	if ((castNode = ExprNode::as<CastNode>(node)))
		return check_for_nod_from(castNode->source);
	else if ((subQueryNode = ExprNode::as<SubQueryNode>(node)) && subQueryNode->blrOp == blr_via)
		return true;

	return false;
}

static SLONG decompose(thread_db* tdbb, BoolExprNode* boolNode, BoolExprNodeStack& stack,
	CompilerScratch* csb)
{
/**************************************
 *
 *	d e c o m p o s e
 *
 **************************************
 *
 * Functional description
 *	Decompose a boolean into a stack of conjuctions.
 *
 **************************************/
	DEV_BLKCHK(csb, type_csb);

	BinaryBoolNode* binaryNode = boolNode->as<BinaryBoolNode>();
	ComparativeBoolNode* cmpNode = boolNode->as<ComparativeBoolNode>();

	if (binaryNode)
	{
		if (binaryNode->blrOp == blr_and)
		{
			SLONG count = decompose(tdbb, binaryNode->arg1, stack, csb);
			count += decompose(tdbb, binaryNode->arg2, stack, csb);
			return count;
		}
		else if (binaryNode->blrOp == blr_or)
		{
			BoolExprNodeStack or_stack;

			if (decompose(tdbb, binaryNode->arg1, or_stack, csb) >= 2)
			{
				binaryNode->arg1 = or_stack.pop();

				while (or_stack.hasData())
				{
					BinaryBoolNode* newBoolNode = FB_NEW(csb->csb_pool) BinaryBoolNode(
						csb->csb_pool, blr_and);
					newBoolNode->arg1 = or_stack.pop();
					newBoolNode->arg2 = binaryNode->arg1;

					binaryNode->arg1 = newBoolNode;
				}
			}

			or_stack.clear();

			if (decompose(tdbb, binaryNode->arg2, or_stack, csb) >= 2)
			{
				binaryNode->arg2 = or_stack.pop();

				while (or_stack.hasData())
				{
					BinaryBoolNode* newBoolNode = FB_NEW(csb->csb_pool) BinaryBoolNode(
						csb->csb_pool, blr_and);
					newBoolNode->arg1 = or_stack.pop();
					newBoolNode->arg2 = binaryNode->arg2;

					binaryNode->arg2 = newBoolNode;
				}
			}
		}
	}
	else if (cmpNode)
	{
		// turn a between into (a greater than or equal) AND (a less than  or equal)

		if (cmpNode->blrOp == blr_between)
		{
			if (check_for_nod_from(cmpNode->arg1))
			{
				// Without this ERR_punt(), server was crashing with sub queries
				// under "between" predicate, Bug No. 73766
				ERR_post(Arg::Gds(isc_optimizer_between_err));
				// Msg 493: Unsupported field type specified in BETWEEN predicate
			}

			ComparativeBoolNode* newCmpNode = FB_NEW(csb->csb_pool) ComparativeBoolNode(
				csb->csb_pool, blr_geq);
			newCmpNode->arg1 = cmpNode->arg1;
			newCmpNode->arg2 = cmpNode->arg2;

			stack.push(newCmpNode);

			newCmpNode = FB_NEW(csb->csb_pool) ComparativeBoolNode(csb->csb_pool, blr_leq);
			newCmpNode->arg1 = CMP_clone_node_opt(tdbb, csb, cmpNode->arg1);
			newCmpNode->arg2 = cmpNode->arg3;

			stack.push(newCmpNode);

			return 2;
		}

		// turn a LIKE into a LIKE and a STARTING WITH, if it starts
		// with anything other than a pattern-matching character

		jrd_nod* arg;

		if (cmpNode->blrOp == blr_like && (arg = optimize_like(tdbb, csb, cmpNode)))
		{
			ComparativeBoolNode* newCmpNode = FB_NEW(csb->csb_pool) ComparativeBoolNode(
				csb->csb_pool, blr_starting);
			newCmpNode->arg1 = cmpNode->arg1;
			newCmpNode->arg2 = arg;

			stack.push(newCmpNode);
			stack.push(boolNode);

			return 2;
		}
	}

	stack.push(boolNode);

	return 1;
}


static USHORT distribute_equalities(BoolExprNodeStack& org_stack, CompilerScratch* csb,
	USHORT base_count)
{
/**************************************
 *
 *	d i s t r i b u t e _ e q u a l i t i e s
 *
 **************************************
 *
 * Functional description
 *	Given a stack of conjunctions, generate some simple
 *	inferences.  In general, find classes of equalities,
 *	then find operations based on members of those classes.
 *	If we find any, generate additional conjunctions.  In
 *	short:
 *
 *		If (a == b) and (a $ c) --> (b $ c) for any
 *		operation '$'.
 *
 **************************************/
	/*thread_db* tdbb = */JRD_get_thread_data();

	ObjectsArray<NodeStack> classes;
	ObjectsArray<NodeStack>::iterator eq_class;

	DEV_BLKCHK(csb, type_csb);

	// Zip thru stack of booleans looking for field equalities

	for (BoolExprNodeStack::iterator stack1(org_stack); stack1.hasData(); ++stack1)
	{
		BoolExprNode* boolean = stack1.object();

		if (boolean->flags & BoolExprNode::FLAG_DEOPTIMIZE)
			continue;

		ComparativeBoolNode* cmpNode = boolean->as<ComparativeBoolNode>();

		if (!cmpNode || cmpNode->blrOp != blr_eql)
			continue;

		jrd_nod* node1 = cmpNode->arg1;
		if (!ExprNode::is<FieldNode>(node1))
			continue;

		jrd_nod* node2 = cmpNode->arg2;
		if (!ExprNode::is<FieldNode>(node2))
			continue;

		for (eq_class = classes.begin(); eq_class != classes.end(); ++eq_class)
		{
			if (search_stack(node1, *eq_class))
			{
				augment_stack(node2, *eq_class);
				break;
			}
			else if (search_stack(node2, *eq_class))
			{
				eq_class->push(node1);
				break;
			}
		}

		if (eq_class == classes.end())
		{
			NodeStack& s = classes.add();
			s.push(node1);
			s.push(node2);
			eq_class = classes.back();
		}
	}

	if (classes.getCount() == 0)
		return 0;

	// Make another pass looking for any equality relationships that may have crept
	// in between classes (this could result from the sequence (A = B, C = D, B = C)

	for (eq_class = classes.begin(); eq_class != classes.end(); ++eq_class)
	{
		for (NodeStack::const_iterator stack2(*eq_class); stack2.hasData(); ++stack2)
		{
			for (ObjectsArray<NodeStack>::iterator eq_class2(eq_class);
				 ++eq_class2 != classes.end();)
			{
				if (search_stack(stack2.object(), *eq_class2))
				{
					while (eq_class2->hasData())
						augment_stack(eq_class2->pop(), *eq_class);
				}
			}
		}
	}

	USHORT count = 0;

	// Start by making a pass distributing field equalities

	for (eq_class = classes.begin(); eq_class != classes.end(); ++eq_class)
	{
		if (eq_class->hasMore(2))
		{
			for (NodeStack::iterator outer(*eq_class); outer.hasData(); ++outer)
			{
				for (NodeStack::iterator inner(outer); (++inner).hasData(); )
				{
					ComparativeBoolNode* cmpNode =
						FB_NEW(csb->csb_pool) ComparativeBoolNode(csb->csb_pool, blr_eql);
					cmpNode->arg1 = outer.object();
					cmpNode->arg2 = inner.object();

					if ((base_count + count < MAX_CONJUNCTS) && augment_stack(cmpNode, org_stack))
						count++;
					else
						delete cmpNode;
				}
			}
		}
	}

	// Now make a second pass looking for non-field equalities

	for (BoolExprNodeStack::iterator stack3(org_stack); stack3.hasData(); ++stack3)
	{
		BoolExprNode* boolean = stack3.object();
		ComparativeBoolNode* cmpNode = boolean->as<ComparativeBoolNode>();
		jrd_nod* node1;
		jrd_nod* node2;

		if (cmpNode &&
			(cmpNode->blrOp == blr_eql || cmpNode->blrOp == blr_gtr || cmpNode->blrOp == blr_geq ||
			 cmpNode->blrOp == blr_leq || cmpNode->blrOp == blr_lss ||
			 cmpNode->blrOp == blr_matching || cmpNode->blrOp == blr_containing ||
			 cmpNode->blrOp == blr_like || cmpNode->blrOp == blr_similar))
		{
			node1 = cmpNode->arg1;
			node2 = cmpNode->arg2;
		}
		else
			continue;

		bool reverse = false;

		if (!ExprNode::is<FieldNode>(node1))
		{
			jrd_nod* swap_node = node1;
			node1 = node2;
			node2 = swap_node;
			reverse = true;
		}

		if (!ExprNode::is<FieldNode>(node1))
			continue;

		if (!ExprNode::is<LiteralNode>(node2) &&
			!ExprNode::is<ParameterNode>(node2) &&
			!ExprNode::is<VariableNode>(node2))
		{
			continue;
		}

		for (eq_class = classes.begin(); eq_class != classes.end(); ++eq_class)
		{
			if (search_stack(node1, *eq_class))
			{
				for (NodeStack::iterator temp(*eq_class); temp.hasData(); ++temp)
				{
					if (!node_equality(node1, temp.object()))
					{
						jrd_nod* arg1;
						jrd_nod* arg2;

						if (reverse)
						{
							arg1 = cmpNode->arg1;
							arg2 = temp.object();
						}
						else
						{
							arg1 = temp.object();
							arg2 = cmpNode->arg2;
						}

						// From the conjuncts X(A,B) and A=C, infer the conjunct X(C,B)
						BoolExprNode* newNode = make_inference_node(csb, boolean, arg1, arg2);

						if ((base_count + count < MAX_CONJUNCTS) && augment_stack(newNode, org_stack))
							++count;
					}
				}

				break;
			}
		}
	}

	return count;
}


static void find_index_relationship_streams(thread_db* tdbb,
											OptimizerBlk* opt,
											const UCHAR* streams,
											UCHAR* dependent_streams,
											UCHAR* free_streams)
{
/**************************************
 *
 *	f i n d _ i n d e x _ r e l a t i o n s h i p _ s t r e a m s
 *
 **************************************
 *
 * Functional description
 *	Find the streams that can use an index
 *	with the currently active streams.
 *
 **************************************/

	DEV_BLKCHK(opt, type_opt);
	SET_TDBB(tdbb);

	CompilerScratch* const csb = opt->opt_csb;
	const UCHAR* end_stream = streams + 1 + streams[0];
	for (const UCHAR* stream = streams + 1; stream < end_stream; stream++)
	{
		CompilerScratch::csb_repeat* const csb_tail = &csb->csb_rpt[*stream];

		// Set temporary active flag for this stream
		csb_tail->csb_flags |= csb_active;

		bool indexed_relationship = false;

		if (opt->opt_conjuncts.getCount())
		{
			// Calculate the inversion for this stream.
			// The returning candidate contains the streams that will be used for
			// index retrieval. This meant that if some stream is used this stream
			// depends on already active streams and can not be used in a separate
			// SORT/MERGE.

			OptimizerRetrieval optimizerRetrieval(*tdbb->getDefaultPool(), opt, *stream, false, false, NULL);

			AutoPtr<InversionCandidate> candidate(optimizerRetrieval.getCost());

			if (candidate->dependentFromStreams.hasData())
			{
				indexed_relationship = true;
			}
		}

		if (indexed_relationship)
		{
			dependent_streams[++dependent_streams[0]] = *stream;
		}
		else
		{
			free_streams[++free_streams[0]] = *stream;
		}

		// Reset active flag
		csb_tail->csb_flags &= ~csb_active;
	}
}


static void form_rivers(thread_db*		tdbb,
						OptimizerBlk*	opt,
						const UCHAR*	streams,
						RiverList&		river_list,
						SortNode**		sort_clause,
						PlanNode*		plan_clause)
{
/**************************************
 *
 *	f o r m _ r i v e r s
 *
 **************************************
 *
 * Functional description
 *	Form streams into rivers according
 *	to the user-specified plan.
 *
 **************************************/
	SET_TDBB(tdbb);
	DEV_BLKCHK(opt, type_opt);

	stream_array_t temp;
	temp[0] = 0;

	// this must be a join or a merge node, so go through
	// the substreams and place them into the temp vector
	// for formation into a river.
	PlanNode* plan_node = NULL;
	NestConst<PlanNode>* ptr = plan_clause->subNodes.begin();

	for (const NestConst<PlanNode>* const end = plan_clause->subNodes.end(); ptr != end; ++ptr)
	{
		plan_node = *ptr;

		if (plan_node->type == PlanNode::TYPE_JOIN)
		{
			form_rivers(tdbb, opt, streams, river_list, sort_clause, plan_node);
			continue;
		}

		// at this point we must have a retrieval node, so put
		// the stream into the river.
		fb_assert(plan_node->type == PlanNode::TYPE_RETRIEVE);

		const UCHAR stream = plan_node->relationNode->getStream();

		// dimitr:	the plan may contain more retrievals than the "streams"
		//			array (some streams could already be joined to the active
		//			rivers), so we populate the "temp" array only with the
		//			streams that appear in both the plan and the "streams"
		//			array.

		const UCHAR* ptr_stream = streams + 1;
		const UCHAR* const end_stream = ptr_stream + streams[0];

		while (ptr_stream < end_stream)
		{
			if (*ptr_stream++ == stream)
			{
				temp[0]++;
				temp[temp[0]] = stream;
				break;
			}
		}
	}

	// just because the user specified a join does not mean that
	// we are able to form a river;  thus form as many rivers out
	// of the join are as necessary to exhaust the streams.
	// AB: Only form rivers when any retrieval node is seen, for
	// example a MERGE on two JOINs will come with no retrievals
	// at this point.
	// CVC: Notice "plan_node" is pointing to the last element in the loop above.
	// If the loop didn't execute, we had garbage in "plan_node".

	if (temp[0] != 0)
	{
		OptimizerInnerJoin* const innerJoin = FB_NEW(*tdbb->getDefaultPool())
			OptimizerInnerJoin(*tdbb->getDefaultPool(), opt, temp, sort_clause, plan_clause);
		USHORT count;

		do {
			count = innerJoin->findJoinOrder();
		} while (form_river(tdbb, opt, count, streams[0], temp, river_list, sort_clause));

		delete innerJoin;
	}
}


static bool form_river(thread_db*		tdbb,
					   OptimizerBlk*	opt,
					   USHORT			count,
					   USHORT			stream_count,
					   UCHAR*			temp,
					   RiverList&		river_list,
					   SortNode** sort_clause)
{
/**************************************
 *
 *	f o r m _ r i v e r
 *
 **************************************
 *
 * Functional description
 *	Form streams into rivers (combinations of streams).
 *
 **************************************/
	fb_assert(count);

	DEV_BLKCHK(opt, type_opt);
	DEV_BLKCHK(plan_clause, type_nod);

	SET_TDBB(tdbb);

	CompilerScratch* const csb = opt->opt_csb;

	HalfStaticArray<RecordSource*, OPT_STATIC_ITEMS> rsbs;
	rsbs.resize(count);
	RecordSource** ptr = rsbs.begin();

	StreamList streams;
	streams.resize(count);
	UCHAR* stream = streams.begin();

	if (count != stream_count)
	{
		sort_clause = NULL;
	}

	const OptimizerBlk::opt_stream* const opt_end = opt->opt_streams.begin() + count;
	for (OptimizerBlk::opt_stream* tail = opt->opt_streams.begin();
		 tail < opt_end; tail++, stream++, ptr++)
	{
		*stream = (UCHAR) tail->opt_best_stream;
		*ptr = gen_retrieval(tdbb, opt, *stream, sort_clause, false, false, NULL);
		sort_clause = NULL;
	}

	RecordSource* const rsb = (count == 1) ? rsbs[0] :
		FB_NEW(*tdbb->getDefaultPool()) NestedLoopJoin(csb, count, rsbs.begin());

	// Allocate a river block and move the best order into it
	River* const river = FB_NEW(*tdbb->getDefaultPool()) River(csb, rsb, NULL, count, streams.begin());
	river->deactivate(csb);
	river_list.push(river);

	stream = temp + 1;
	const UCHAR* const end_stream = stream + temp[0];

	if (!(temp[0] -= count))
	{
		return false;
	}

	// Reform "temp" from streams not consumed
	for (const UCHAR* t2 = stream; t2 < end_stream; t2++)
	{
		bool used = false;

		for (OptimizerBlk::opt_stream* tail = opt->opt_streams.begin(); tail < opt_end; tail++)
		{
			if (*t2 == tail->opt_best_stream)
			{
				used = true;
				break;
			}
		}

		if (!used)
		{
			*stream++ = *t2;
		}
	}

	return true;
}


// Generate a separate AggregateSort (Aggregate SortedStream Block) for each distinct operation.
// Note that this should be optimized to use indices if possible.
void OPT_gen_aggregate_distincts(thread_db* tdbb, CompilerScratch* csb, MapNode* map)
{
	DSC descriptor;
	DSC* desc = &descriptor;
	NestConst<jrd_nod>* ptr = map->items.begin();

	for (const NestConst<jrd_nod>* const end = map->items.end(); ptr != end; ++ptr)
	{
		jrd_nod* from = (*ptr)->nod_arg[e_asgn_from];
		AggNode* aggNode = ExprNode::as<AggNode>(from);

		if (aggNode && aggNode->distinct)
		{
			// Build the sort key definition. Turn cstrings into varying text.
			CMP_get_desc(tdbb, csb, aggNode->arg, desc);

			if (desc->dsc_dtype == dtype_cstring)
			{
				desc->dsc_dtype = dtype_varying;
				desc->dsc_length++;
			}

			AggregateSort* asb = FB_NEW(*tdbb->getDefaultPool()) AggregateSort(
				*tdbb->getDefaultPool());
			asb->intl = desc->isText() && desc->getTextType() != ttype_none &&
				desc->getTextType() != ttype_binary && desc->getTextType() != ttype_ascii;

			sort_key_def* sort_key = asb->keyItems.getBuffer(asb->intl ? 2 : 1);
			sort_key->skd_offset = 0;

			if (asb->intl)
			{
				const USHORT key_length = ROUNDUP(INTL_key_length(tdbb,
					INTL_TEXT_TO_INDEX(desc->getTextType()), desc->getStringLength()), sizeof(SINT64));

				sort_key->skd_dtype = SKD_bytes;
				sort_key->skd_flags = SKD_ascending;
				sort_key->skd_length = key_length;
				sort_key->skd_offset = 0;
				sort_key->skd_vary_offset = 0;

				++sort_key;
				asb->length = sort_key->skd_offset = key_length;
			}

			fb_assert(desc->dsc_dtype < FB_NELEM(sort_dtypes));
			sort_key->skd_dtype = sort_dtypes[desc->dsc_dtype];
			if (!sort_key->skd_dtype)
			{
				ERR_post(Arg::Gds(isc_invalid_sort_datatype) << Arg::Str(DSC_dtype_tostring(desc->dsc_dtype)));
			}

			sort_key->skd_length = desc->dsc_length;

			if (desc->dsc_dtype == dtype_varying)
			{
				// allocate space to store varying length
				sort_key->skd_vary_offset = sort_key->skd_offset + ROUNDUP(desc->dsc_length, sizeof(SLONG));
				asb->length = sort_key->skd_vary_offset + sizeof(USHORT);
			}
			else
				asb->length += sort_key->skd_length;

			sort_key->skd_flags = SKD_ascending;
			asb->impure = CMP_impure(csb, sizeof(impure_agg_sort));
			asb->desc = *desc;

			aggNode->asb = asb;
		}
	}
}


static void gen_join(thread_db*		tdbb,
					 OptimizerBlk*	opt,
					 const UCHAR*	streams,
					 RiverList&		river_list,
					 SortNode** sort_clause,
					 PlanNode*		plan_clause)
{
/**************************************
 *
 *	g e n _ j o i n
 *
 **************************************
 *
 * Functional description
 *	Find all indexed relationships between streams,
 *	then form streams into rivers (combinations of
 * 	streams).
 *
 **************************************/
	DEV_BLKCHK(opt, type_opt);
	SET_TDBB(tdbb);

	if (!streams[0])
		return;

	if (plan_clause && streams[0] > 1)
	{
		// this routine expects a join/merge
		form_rivers(tdbb, opt, streams, river_list, sort_clause, plan_clause);
		return;
	}

	OptimizerInnerJoin* const innerJoin = FB_NEW(*tdbb->getDefaultPool())
		OptimizerInnerJoin(*tdbb->getDefaultPool(), opt, streams, sort_clause, plan_clause);

	stream_array_t temp;
	memcpy(temp, streams, streams[0] + 1);

	USHORT count;
	do {
		count = innerJoin->findJoinOrder();
	} while (form_river(tdbb, opt, count, streams[0], temp, river_list, sort_clause));

	delete innerJoin;
}


static RecordSource* gen_outer(thread_db* tdbb, OptimizerBlk* opt, RseNode* rse,
	RiverList& river_list, SortNode** sort_clause)
{
/**************************************
 *
 *	g e n _ o u t e r
 *
 **************************************
 *
 * Functional description
 *	Generate a top level outer join.  The "outer" and "inner"
 *	sub-streams must be handled differently from each other.
 *	The inner is like other streams.  The outer stream isn't
 *	because conjuncts may not eliminate records from the
 *	stream.  They only determine if a join with an inner
 *	stream record is to be attempted.
 *
 **************************************/
	struct {
		RecordSource* stream_rsb;
		USHORT stream_num;
	} stream_o, stream_i, *stream_ptr[2];

	DEV_BLKCHK(opt, type_opt);
	DEV_BLKCHK(rse, type_nod);
	SET_TDBB(tdbb);

	// Determine which stream should be outer and which is inner.
	// In the case of a left join, the syntactically left stream is the
	// outer, and the right stream is the inner.  For all others, swap
	// the sense of inner and outer, though for a full join it doesn't
	// matter and we should probably try both orders to see which is
	// more efficient.
	if (rse->rse_jointype != blr_left)
	{
		stream_ptr[1] = &stream_o;
		stream_ptr[0] = &stream_i;
	}
	else
	{
		stream_ptr[0] = &stream_o;
		stream_ptr[1] = &stream_i;
	}

	// Loop through the outer join sub-streams in
	// reverse order because rivers may have been PUSHed
	for (int i = 1; i >= 0; i--)
	{
		const RecordSourceNode* node = rse->rse_relations[i];

		if (node->type == RelationSourceNode::TYPE)
		{
			stream_ptr[i]->stream_rsb = NULL;
			stream_ptr[i]->stream_num = node->getStream();
		}
		else
		{
			River* const river = river_list.pop();
			stream_ptr[i]->stream_rsb = river->getRecordSource();
		}
	}

	CompilerScratch* const csb = opt->opt_csb;

	const bool isFullJoin = (rse->rse_jointype == blr_full);

	if (!isFullJoin)
	{
		// Generate rsbs for the sub-streams.
		// For the left sub-stream we also will get a boolean back.
		BoolExprNode* boolean = NULL;

		if (!stream_o.stream_rsb)
		{
			stream_o.stream_rsb = gen_retrieval(tdbb, opt, stream_o.stream_num, sort_clause,
												true, false, &boolean);
		}

		if (!stream_i.stream_rsb)
		{
			// AB: the sort clause for the inner stream of an OUTER JOIN
			//	   should never be used for the index retrieval
			stream_i.stream_rsb =
				gen_retrieval(tdbb, opt, stream_i.stream_num, NULL, false, true, NULL);
		}

		// generate a parent boolean rsb for any remaining booleans that
		// were not satisfied via an index lookup
		stream_i.stream_rsb = gen_residual_boolean(tdbb, opt, stream_i.stream_rsb);

		// Allocate and fill in the rsb
		return FB_NEW(*tdbb->getDefaultPool())
			NestedLoopJoin(csb, stream_o.stream_rsb, stream_i.stream_rsb, boolean, false, false);
	}

	bool hasOuterRsb = true, hasInnerRsb = true;
	BoolExprNode* boolean = NULL;

	if (!stream_o.stream_rsb)
	{
		hasOuterRsb = false;
		stream_o.stream_rsb =
			gen_retrieval(tdbb, opt, stream_o.stream_num, NULL, true, false, &boolean);
	}

	if (!stream_i.stream_rsb)
	{
		hasInnerRsb = false;
		stream_i.stream_rsb =
			gen_retrieval(tdbb, opt, stream_i.stream_num, NULL, false, true, NULL);
	}

	stream_i.stream_rsb = gen_residual_boolean(tdbb, opt, stream_i.stream_rsb);

	RecordSource* const rsb1 = FB_NEW(*tdbb->getDefaultPool())
		NestedLoopJoin(csb, stream_o.stream_rsb, stream_i.stream_rsb, boolean, false, false);

	for (size_t i = 0; i < opt->opt_conjuncts.getCount(); i++)
	{
		if (opt->opt_conjuncts[i].opt_conjunct_flags & opt_conjunct_used)
		{
			BoolExprNode* const org_node = opt->opt_conjuncts[i].opt_conjunct_node;
			opt->opt_conjuncts[i].opt_conjunct_node = CMP_clone_node_opt(tdbb, csb, org_node);
			opt->opt_conjuncts[i].opt_conjunct_flags = 0;
		}
	}

	if (!hasInnerRsb)
		csb->csb_rpt[stream_i.stream_num].csb_flags &= ~csb_active;

	if (!hasOuterRsb)
		csb->csb_rpt[stream_o.stream_num].csb_flags &= ~csb_active;

	if (!hasInnerRsb)
	{
		stream_i.stream_rsb =
			gen_retrieval(tdbb, opt, stream_i.stream_num, NULL, false, false, NULL);
	}

	if (!hasOuterRsb)
	{
		stream_o.stream_rsb =
			gen_retrieval(tdbb, opt, stream_o.stream_num, NULL, false, false, NULL);
	}

	stream_o.stream_rsb = gen_residual_boolean(tdbb, opt, stream_o.stream_rsb);

	RecordSource* const rsb2 = FB_NEW(*tdbb->getDefaultPool())
		NestedLoopJoin(csb, stream_i.stream_rsb, stream_o.stream_rsb, NULL, false, true);

	return FB_NEW(*tdbb->getDefaultPool()) FullOuterJoin(csb, rsb1, rsb2);
}


static RecordSource* gen_residual_boolean(thread_db* tdbb, OptimizerBlk* opt, RecordSource* prior_rsb)
{
/**************************************
 *
 *	g e n _ r e s i d u a l _ b o o l e a n
 *
 **************************************
 *
 * Functional description
 *	Pick up any residual boolean remaining,
 *	meaning those that have not been used
 *	as part of some join.  These booleans
 *	must still be applied to the result stream.
 *
 **************************************/
	SET_TDBB(tdbb);
	DEV_BLKCHK(opt, type_opt);
	DEV_BLKCHK(prior_rsb, type_rsb);

	BoolExprNode* boolean = NULL;
	const OptimizerBlk::opt_conjunct* const opt_end =
		opt->opt_conjuncts.begin() + opt->opt_base_conjuncts;

	for (OptimizerBlk::opt_conjunct* tail = opt->opt_conjuncts.begin(); tail < opt_end; tail++)
	{
		BoolExprNode* node = tail->opt_conjunct_node;

		if (!(tail->opt_conjunct_flags & opt_conjunct_used))
		{
			compose(tdbb, &boolean, node);
			tail->opt_conjunct_flags |= opt_conjunct_used;
		}
	}

	return boolean ?
		FB_NEW(*tdbb->getDefaultPool()) FilteredStream(opt->opt_csb, prior_rsb, boolean) :
		prior_rsb;
}


static RecordSource* gen_retrieval(thread_db*     tdbb,
						 OptimizerBlk*      opt,
						 SSHORT   stream,
						 SortNode** sort_ptr,
						 bool     outer_flag,
						 bool     inner_flag,
						 BoolExprNode** return_boolean)
{
/**************************************
 *
 *	g e n _ r e t r i e v a l
 *
 **************************************
 *
 * Functional description
 *	Compile and optimize a record selection expression into a
 *	set of record source blocks (rsb's).
 *
 **************************************/
	OptimizerBlk::opt_conjunct* tail;

	SET_TDBB(tdbb);

	DEV_BLKCHK(opt, type_opt);

	CompilerScratch* const csb = opt->opt_csb;
	CompilerScratch::csb_repeat* const csb_tail = &csb->csb_rpt[stream];
	jrd_rel* const relation = csb_tail->csb_relation;

	fb_assert(relation);

	const string alias = OPT_make_alias(tdbb, csb, csb_tail);
	csb_tail->csb_flags |= csb_active;

	// Time to find inversions. For each index on the relation
	// match all unused booleans against the index looking for upper
	// and lower bounds that can be computed by the index. When
	// all unused conjunctions are exhausted, see if there is enough
	// information for an index retrieval. If so, build up an
	// inversion component of the boolean.

	// It's recalculated later.
	const OptimizerBlk::opt_conjunct* opt_end = opt->opt_conjuncts.begin() +
		(inner_flag ? opt->opt_base_missing_conjuncts : opt->opt_conjuncts.getCount());

	RecordSource* rsb = NULL;
	IndexTableScan* nav_rsb = NULL;
	InversionNode* inversion = NULL;

	if (relation->rel_file)
	{
		// External table
		rsb = FB_NEW(*tdbb->getDefaultPool()) ExternalTableScan(csb, alias, stream);
	}
	else if (relation->isVirtual())
	{
		// Virtual table: monitoring or security
		if (relation->rel_id == rel_sec_users)
		{
			rsb = FB_NEW(*tdbb->getDefaultPool()) UsersTableScan(csb, alias, stream);
		}
		else
		{
			rsb = FB_NEW(*tdbb->getDefaultPool()) MonitoringTableScan(csb, alias, stream);
		}
	}
	else
	{
		// Persistent table
		OptimizerRetrieval optimizerRetrieval(*tdbb->getDefaultPool(),
												opt, stream, outer_flag, inner_flag, sort_ptr);
		AutoPtr<InversionCandidate> candidate(optimizerRetrieval.getInversion(&nav_rsb));

		if (candidate && candidate->inversion)
			inversion = candidate->inversion;
	}

	if (outer_flag)
	{
		fb_assert(return_boolean);
		// Now make another pass thru the outer conjuncts only, finding unused,
		// computable booleans.  When one is found, roll it into a final
		// boolean and mark it used.
		*return_boolean = NULL;
		opt_end = opt->opt_conjuncts.begin() + opt->opt_base_conjuncts;

		for (tail = opt->opt_conjuncts.begin(); tail < opt_end; tail++)
		{
			BoolExprNode* node = tail->opt_conjunct_node;

			if (!(tail->opt_conjunct_flags & opt_conjunct_used) &&
				node->computable(csb, -1, false, false))
			{
				compose(tdbb, return_boolean, node);
				tail->opt_conjunct_flags |= opt_conjunct_used;
			}
		}
	}

	// Now make another pass thru the conjuncts finding unused, computable
	// booleans.  When one is found, roll it into a final boolean and mark
	// it used. If a computable boolean didn't match against an index then
	// mark the stream to denote unmatched booleans.
	BoolExprNode* boolean = NULL;
	opt_end = opt->opt_conjuncts.begin() + (inner_flag ? opt->opt_base_missing_conjuncts : opt->opt_conjuncts.getCount());
	tail = opt->opt_conjuncts.begin();

	if (outer_flag)
		tail += opt->opt_base_parent_conjuncts;

	for (; tail < opt_end; tail++)
	{
		BoolExprNode* const node = tail->opt_conjunct_node;

		if (!(tail->opt_conjunct_flags & opt_conjunct_used) &&
			node->computable(csb, -1, false, false))
		{
			StreamFinder finder(csb, stream);

			// If no index is used then leave other nodes alone, because they
			// could be used for building a SORT/MERGE.
			if ((inversion && node->jrdStreamFinder(finder)) ||
				(!inversion && node->computable(csb, stream, false, true)))
			{
				compose(tdbb, &boolean, node);
				tail->opt_conjunct_flags |= opt_conjunct_used;

				if (!outer_flag && !(tail->opt_conjunct_flags & opt_conjunct_matched))
					csb_tail->csb_flags |= csb_unmatched;
			}
		}
	}

	if (nav_rsb)
	{
		nav_rsb->setInversion(inversion);
		fb_assert(!rsb);
		rsb = nav_rsb;
	}

	if (!rsb)
	{
		if (inversion)
			rsb = FB_NEW(*tdbb->getDefaultPool()) BitmapTableScan(csb, alias, stream, inversion);
		else
		{
			rsb = FB_NEW(*tdbb->getDefaultPool()) FullTableScan(csb, alias, stream);

			if (boolean)
				csb->csb_rpt[stream].csb_flags |= csb_unmatched;
		}
	}

	return boolean ? FB_NEW(*tdbb->getDefaultPool()) FilteredStream(csb, rsb, boolean) : rsb;
}


SortedStream* OPT_gen_sort(thread_db* tdbb, CompilerScratch* csb, const UCHAR* streams,
	const UCHAR* dbkey_streams, RecordSource* prior_rsb, SortNode* sort, bool project_flag)
{
/**************************************
 *
 *	g e n _ s o r t
 *
 **************************************
 *
 * Functional description
 *	Generate a record source block to handle either a sort or a project.
 *	The two case are virtual identical -- the only difference is that
 *	project eliminates duplicates.  However, since duplicates are
 *	recognized and handled by sort, the JRD processing is identical.
 *
 **************************************/
	DEV_BLKCHK(prior_rsb, type_rsb);
	SET_TDBB(tdbb);

	/* We already know the number of keys, but we also need to compute the
	total number of fields, keys and non-keys, to be pumped thru sort.  Starting
	with the number of keys, count the other field referenced.  Since a field
	is often a key, check for overlap to keep the length of the sort record
	down. */

	/* Along with the record number, the transaction id of the
	 * record will also be stored in the sort file.  This will
	 * be used to detect update conflict in read committed
	 * transactions. */

	const UCHAR* ptr;
	dsc descriptor;

	ULONG items = sort->expressions.getCount() +
		(streams[0] * 3) + 2 * (dbkey_streams ? dbkey_streams[0] : 0);
	const UCHAR* const end_ptr = streams + streams[0];
	const NestConst<jrd_nod>* const end_node = sort->expressions.end();
	HalfStaticArray<SLONG, OPT_STATIC_ITEMS> id_list;
	StreamList stream_list;

	for (ptr = &streams[1]; ptr <= end_ptr; ptr++)
	{
		UInt32Bitmap::Accessor accessor(csb->csb_rpt[*ptr].csb_fields);

		if (accessor.getFirst())
		{
			do
			{
				const ULONG id = accessor.current();
				items++;
				id_list.push(id);
				stream_list.push(*ptr);

				for (NestConst<jrd_nod>* node_ptr = sort->expressions.begin();
					 node_ptr != end_node;
					 ++node_ptr)
				{
					FieldNode* fieldNode = ExprNode::as<FieldNode>(node_ptr->getObject());

					if (fieldNode && fieldNode->fieldStream == *ptr && fieldNode->fieldId == id)
					{
						dsc* desc = &descriptor;
						fieldNode->getDesc(tdbb, csb, desc);

						// International type text has a computed key
						if (IS_INTL_DATA(desc))
							break;

						--items;
						id_list.pop();
						stream_list.pop();

						break;
					}
				}
			} while (accessor.getNext());
		}
	}

	if (items > MAX_USHORT)
		ERR_post(Arg::Gds(isc_imp_exc));

	// Now that we know the number of items, allocate a sort map block.
	SortedStream::SortMap* map =
		FB_NEW(*tdbb->getDefaultPool()) SortedStream::SortMap(*tdbb->getDefaultPool());

	if (project_flag)
		map->flags |= SortedStream::FLAG_PROJECT;

	if (sort->unique)
		map->flags |= SortedStream::FLAG_UNIQUE;

    ULONG map_length = 0;

	// Loop thru sort keys building sort keys.  Actually, to handle null values
	// correctly, two sort keys are made for each field, one for the null flag
	// and one for field itself.

	SortedStream::SortMap::Item* map_item = map->items.getBuffer((USHORT) items);
	sort_key_def* sort_key = map->keyItems.getBuffer(2 * sort->expressions.getCount());
	int* nullOrder = sort->nullOrder.begin();
	bool* descending = sort->descending.begin();

	for (NestConst<jrd_nod>* node_ptr = sort->expressions.begin();
		 node_ptr != end_node;
		 ++node_ptr, ++nullOrder, ++descending, ++map_item)
	{
		// Pick up sort key expression.

		NestConst<jrd_nod> node = *node_ptr;
		dsc* desc = &descriptor;
		CMP_get_desc(tdbb, csb, node, desc);

		// Allow for "key" forms of International text to grow
		if (IS_INTL_DATA(desc))
		{
			// Turn varying text and cstrings into text.

			if (desc->dsc_dtype == dtype_varying)
			{
				desc->dsc_dtype = dtype_text;
				desc->dsc_length -= sizeof(USHORT);
			}
			else if (desc->dsc_dtype == dtype_cstring)
			{
				desc->dsc_dtype = dtype_text;
				desc->dsc_length--;
			}

			desc->dsc_length = INTL_key_length(tdbb, INTL_INDEX_TYPE(desc), desc->dsc_length);
		}

		// Make key for null flag

#ifndef WORDS_BIGENDIAN
		map_length = ROUNDUP(map_length, sizeof(SLONG));
#endif
		const USHORT flag_offset = (USHORT) map_length++;
		sort_key->skd_offset = flag_offset;
		sort_key->skd_dtype = SKD_text;
		sort_key->skd_length = 1;
		// Handle nulls placement
		sort_key->skd_flags = SKD_ascending;

		// Have SQL-compliant nulls ordering for ODS11+
		if ((*nullOrder == rse_nulls_default && !*descending) || *nullOrder == rse_nulls_first)
			sort_key->skd_flags |= SKD_descending;

		++sort_key;

		// Make key for sort key proper
#ifndef WORDS_BIGENDIAN
		map_length = ROUNDUP(map_length, sizeof(SLONG));
#else
		if (desc->dsc_dtype >= dtype_aligned)
			map_length = FB_ALIGN(map_length, type_alignments[desc->dsc_dtype]);
#endif

		sort_key->skd_offset = (USHORT) map_length;
		sort_key->skd_flags = SKD_ascending;
		if (*descending)
			sort_key->skd_flags |= SKD_descending;
		fb_assert(desc->dsc_dtype < FB_NELEM(sort_dtypes));
		sort_key->skd_dtype = sort_dtypes[desc->dsc_dtype];

		if (!sort_key->skd_dtype)
			ERR_post(Arg::Gds(isc_invalid_sort_datatype) << Arg::Str(DSC_dtype_tostring(desc->dsc_dtype)));

		if (sort_key->skd_dtype == SKD_varying || sort_key->skd_dtype == SKD_cstring)
		{
			if (desc->dsc_ttype() == ttype_binary)
				sort_key->skd_flags |= SKD_binary;
		}

		sort_key->skd_length = desc->dsc_length;
		++sort_key;
		map_item->clear();
		map_item->node = node;
		map_item->flagOffset = flag_offset;
		map_item->desc = *desc;
		map_item->desc.dsc_address = (UCHAR*)(IPTR) map_length;
		map_length += desc->dsc_length;

		FieldNode* fieldNode;

		if ((fieldNode = ExprNode::as<FieldNode>(node.getObject())))
		{
			map_item->stream = fieldNode->fieldStream;
			map_item->fieldId = fieldNode->fieldId;
		}
	}

	map_length = ROUNDUP(map_length, sizeof(SLONG));
	map->keyLength = (USHORT) map_length >> SHIFTLONG;
	USHORT flag_offset = (USHORT) map_length;
	map_length += items - sort->expressions.getCount();

	// Now go back and process all to fields involved with the sort.  If the
	// field has already been mentioned as a sort key, don't bother to repeat it.

	while (stream_list.hasData())
	{
		const SLONG id = id_list.pop();
		const UCHAR stream = stream_list.pop();
		const Format* format = CMP_format(tdbb, csb, stream);
		const dsc* desc = &format->fmt_desc[id];
		if (id >= format->fmt_count || desc->dsc_dtype == dtype_unknown)
			IBERROR(157);		// msg 157 cannot sort on a field that does not exist
		if (desc->dsc_dtype >= dtype_aligned)
			map_length = FB_ALIGN(map_length, type_alignments[desc->dsc_dtype]);
		map_item->clear();
		map_item->fieldId = (SSHORT) id;
		map_item->stream = stream;
		map_item->flagOffset = flag_offset++;
		map_item->desc = *desc;
		map_item->desc.dsc_address = (UCHAR*)(IPTR) map_length;
		map_length += desc->dsc_length;
		map_item++;
	}

	// Make fields for record numbers record for all streams

	map_length = ROUNDUP(map_length, sizeof(SINT64));
	for (ptr = &streams[1]; ptr <= end_ptr; ptr++, map_item++)
	{
		map_item->clear();
		map_item->fieldId = SortedStream::ID_DBKEY;
		map_item->stream = *ptr;
		dsc* desc = &map_item->desc;
		desc->dsc_dtype = dtype_int64;
		desc->dsc_length = sizeof(SINT64);
		desc->dsc_address = (UCHAR*)(IPTR) map_length;
		map_length += desc->dsc_length;
	}

	// Make fields for transaction id of record for all streams

	for (ptr = &streams[1]; ptr <= end_ptr; ptr++, map_item++)
	{
		map_item->clear();
		map_item->fieldId = SortedStream::ID_TRANS;
		map_item->stream = *ptr;
		dsc* desc = &map_item->desc;
		desc->dsc_dtype = dtype_long;
		desc->dsc_length = sizeof(SLONG);
		desc->dsc_address = (UCHAR*)(IPTR) map_length;
		map_length += desc->dsc_length;
	}

	if (dbkey_streams)
	{
		const UCHAR* const end_ptrL = dbkey_streams + dbkey_streams[0];

		map_length = ROUNDUP(map_length, sizeof(SINT64));
		for (ptr = &dbkey_streams[1]; ptr <= end_ptrL; ptr++, map_item++)
		{
			map_item->clear();
			map_item->fieldId = SortedStream::ID_DBKEY;
			map_item->stream = *ptr;
			dsc* desc = &map_item->desc;
			desc->dsc_dtype = dtype_int64;
			desc->dsc_length = sizeof(SINT64);
			desc->dsc_address = (UCHAR*)(IPTR) map_length;
			map_length += desc->dsc_length;
		}

		for (ptr = &dbkey_streams[1]; ptr <= end_ptrL; ptr++, map_item++)
		{
			map_item->clear();
			map_item->fieldId = SortedStream::ID_DBKEY_VALID;
			map_item->stream = *ptr;
			dsc* desc = &map_item->desc;
			desc->dsc_dtype = dtype_text;
			desc->dsc_ttype() = CS_BINARY;
			desc->dsc_length = 1;
			desc->dsc_address = (UCHAR*)(IPTR) map_length;
			map_length += desc->dsc_length;
		}
	}

	for (ptr = &streams[1]; ptr <= end_ptr; ptr++, map_item++)
	{
		map_item->clear();
		map_item->fieldId = SortedStream::ID_DBKEY_VALID;
		map_item->stream = *ptr;
		dsc* desc = &map_item->desc;
		desc->dsc_dtype = dtype_text;
		desc->dsc_ttype() = CS_BINARY;
		desc->dsc_length = 1;
		desc->dsc_address = (UCHAR*)(IPTR) map_length;
		map_length += desc->dsc_length;
	}

	fb_assert(map_item - map->items.begin() == USHORT(map->items.getCount()));
	fb_assert(sort_key - map->keyItems.begin() == USHORT(map->keyItems.getCount()));

	map_length = ROUNDUP(map_length, sizeof(SLONG));

	// Make fields to store varying and cstring length.

	const sort_key_def* const end_key = sort_key;
	for (sort_key = map->keyItems.begin(); sort_key < end_key; sort_key++)
	{
		fb_assert(sort_key->skd_dtype != 0);
		if (sort_key->skd_dtype == SKD_varying || sort_key->skd_dtype == SKD_cstring)
		{
			sort_key->skd_vary_offset = (USHORT) map_length;
			map_length += sizeof(USHORT);
		}
	}

	if (map_length > MAX_SORT_RECORD)
	{
		ERR_post(Arg::Gds(isc_sort_rec_size_err) << Arg::Num(map_length));
		// Msg438: sort record size of %ld bytes is too big
	}

	map->length = (USHORT) map_length;

	// That was most unpleasant.  Never the less, it's done (except for the debugging).
	// All that remains is to build the record source block for the sort.
	return FB_NEW(*tdbb->getDefaultPool()) SortedStream(csb, prior_rsb, map);
}


static bool gen_equi_join(thread_db* tdbb, OptimizerBlk* opt, RiverList& org_rivers)
{
/**************************************
 *
 *	g e n _ e q u i _ j o i n
 *
 **************************************
 *
 * Functional description
 *	We've got a set of rivers that may or may not be amenable to
 *	a hash join or a sort/merge join, and it's time to find out.
 *	If there are, build an appropriate join RecordSource,
 *	push it on the rsb stack, and update rivers accordingly.
 *	If two or more rivers were successfully joined, return true.
 *	If the whole things is a moby no-op, return false.
 *
 **************************************/
	ULONG selected_rivers[OPT_STREAM_BITS], selected_rivers2[OPT_STREAM_BITS];
	jrd_nod** eq_class;
	DEV_BLKCHK(opt, type_opt);
	SET_TDBB(tdbb);

	CompilerScratch* const csb = opt->opt_csb;

	// Count the number of "rivers" involved in the operation, then allocate
	// a scratch block large enough to hold values to compute equality
	// classes.

	const USHORT cnt = (USHORT) org_rivers.getCount();

	if (cnt < 2)
		return false;

	HalfStaticArray<jrd_nod*, OPT_STATIC_ITEMS> scratch;
	scratch.grow(opt->opt_base_conjuncts * cnt);
	jrd_nod** classes = scratch.begin();

	// Compute equivalence classes among streams. This involves finding groups
	// of streams joined by field equalities.

	jrd_nod** last_class = classes;
	OptimizerBlk::opt_conjunct* tail = opt->opt_conjuncts.begin();
	const OptimizerBlk::opt_conjunct* const end = tail + opt->opt_base_conjuncts;

	for (; tail < end; tail++)
	{
		if (tail->opt_conjunct_flags & opt_conjunct_used)
			continue;

		BoolExprNode* const node = tail->opt_conjunct_node;
		ComparativeBoolNode* cmpNode = node->as<ComparativeBoolNode>();

		if (!cmpNode || (cmpNode->blrOp != blr_eql && cmpNode->blrOp != blr_equiv))
			continue;

		jrd_nod* node1 = cmpNode->arg1;
		jrd_nod* node2 = cmpNode->arg2;

		dsc desc1, desc2;
		CMP_get_desc(tdbb, csb, node1, &desc1);
		CMP_get_desc(tdbb, csb, node2, &desc2);

		if (!DSC_EQUIV(&desc1, &desc2, true) || desc1.isBlob() || desc2.isBlob())
			continue;

		USHORT number1 = 0;
		for (River** iter1 = org_rivers.begin(); iter1 < org_rivers.end(); iter1++, number1++)
		{
			River* const river1 = *iter1;

			if (!river1->isReferenced(node1))
			{
				if (!river1->isReferenced(node2))
					continue;

				jrd_nod* const temp = node1;
				node1 = node2;
				node2 = temp;
			}

			USHORT number2 = number1 + 1;

			for (River** iter2 = iter1 + 1; iter2 < org_rivers.end(); iter2++, number2++)
			{
				River* const river2 = *iter2;

				if (river2->isReferenced(node2))
				{
					for (eq_class = classes; eq_class < last_class; eq_class += cnt)
					{
						if (node_equality(node1, classes[number1]) ||
							node_equality(node2, classes[number2]))
						{
							break;
						}
					}

					eq_class[number1] = node1;
					eq_class[number2] = node2;

					if (eq_class == last_class)
						last_class += cnt;
				}
			}
		}
	}

	// Pick both a set of classes and a set of rivers on which to join.
	// Obviously, if the set of classes is empty, return false
	// to indicate that nothing could be done.

	USHORT river_cnt = 0;
	HalfStaticArray<jrd_nod**, OPT_STATIC_ITEMS> selected_classes(cnt);

	for (eq_class = classes; eq_class < last_class; eq_class += cnt)
	{
		USHORT i = river_count(cnt, eq_class);

		if (i > river_cnt)
		{
			river_cnt = i;
			selected_classes.shrink(0);
			selected_classes.add(eq_class);
			class_mask(cnt, eq_class, selected_rivers);
		}
		else
		{
			class_mask(cnt, eq_class, selected_rivers2);

			for (i = 0; i < OPT_STREAM_BITS; i++)
			{
				if ((selected_rivers[i] & selected_rivers2[i]) != selected_rivers[i])
					break;
			}

			if (i == OPT_STREAM_BITS)
				selected_classes.add(eq_class);
		}
	}

	if (!river_cnt)
		return false;

	// AB: Inactivate currently all streams from every river, because we
	// need to know which nodes are computable between the rivers used
	// for the merge.

	USHORT flag_vector[MAX_STREAMS + 1], *fv;
	UCHAR stream_nr;

	for (stream_nr = 0, fv = flag_vector; stream_nr < csb->csb_n_stream; stream_nr++)
	{
		*fv++ = csb->csb_rpt[stream_nr].csb_flags & csb_active;
		csb->csb_rpt[stream_nr].csb_flags &= ~csb_active;
	}

	HalfStaticArray<RecordSource*, OPT_STATIC_ITEMS> rsbs;
	HalfStaticArray<LegacyNodeArray*, OPT_STATIC_ITEMS> keys;

	// Unconditionally disable merge joins in favor of hash joins.
	// This is a temporary debugging measure.
	bool prefer_merge_over_hash = false;

	// AB: Get the lowest river position from the rivers that are merged

	RiverList rivers_to_merge;
	USHORT lowest_river_position = MAX_USHORT;
	USHORT number = 0;

	for (River** iter = org_rivers.begin(); iter < org_rivers.end(); number++)
	{
		River* const river = *iter;

		if (!(TEST_DEP_BIT(selected_rivers, number)))
		{
			iter++;
			continue;
		}

		if (number < lowest_river_position)
			lowest_river_position = number;

		rivers_to_merge.add(river);
		org_rivers.remove(iter);

		RecordSource* rsb = river->getRecordSource();

		// Apply local river booleans, if any

		river->activate(csb);

		BoolExprNode* river_boolean = NULL;

		for (tail = opt->opt_conjuncts.begin(); tail < end; tail++)
		{
			BoolExprNode* const node = tail->opt_conjunct_node;

			if (!(tail->opt_conjunct_flags & opt_conjunct_used) &&
				node->computable(csb, -1, false, false))
			{
				compose(tdbb, &river_boolean, node);
				tail->opt_conjunct_flags |= opt_conjunct_used;
			}
		}

		river->deactivate(csb);

		if (river_boolean)
			rsb = FB_NEW(*tdbb->getDefaultPool()) FilteredStream(csb, rsb, river_boolean);

		// Collect RSBs and keys to join

		//const size_t selected_count = selected_classes.getCount();

		SortNode* key = FB_NEW(*tdbb->getDefaultPool()) SortNode(*tdbb->getDefaultPool());

		if (prefer_merge_over_hash)
		{
			jrd_nod*** selected_class;

			for (selected_class = selected_classes.begin();
				 selected_class != selected_classes.end(); ++selected_class)
			{
				key->descending.add(false);	// Ascending sort
				key->nullOrder.add(rse_nulls_default);	// Default nulls placement
				key->expressions.add((*selected_class)[number]);
			}

			const size_t stream_count = river->getStreamCount();
			fb_assert(stream_count <= MAX_STREAMS);
			stream_array_t streams;
			streams[0] = (UCHAR) stream_count;
			memcpy(streams + 1, river->getStreams(), stream_count);
			rsb = OPT_gen_sort(tdbb, opt->opt_csb, streams, NULL, rsb, key, false);
		}
		else
		{
			jrd_nod*** selected_class;

			for (selected_class = selected_classes.begin();
				 selected_class != selected_classes.end(); ++selected_class)
			{
				key->expressions.add((*selected_class)[number]);
			}
		}

		// It seems that rivers are already sorted by their cardinality.
		// For a hash join, we need to choose the smallest ones as inner sub-streams,
		// hence we reverse the order when storing them in the temporary arrays.

		if (prefer_merge_over_hash)
		{
			rsbs.add(rsb);
			keys.add(&key->expressions);
		}
		else
		{
			rsbs.insert(0, rsb);
			keys.insert(0, &key->expressions);
		}
	}

	fb_assert(rsbs.getCount() == keys.getCount());

	// Build a join stream

	RecordSource* rsb = NULL;

	if (prefer_merge_over_hash)
	{
		rsb = FB_NEW(*tdbb->getDefaultPool())
			MergeJoin(csb, rsbs.getCount(), (SortedStream**) rsbs.begin(), keys.begin());
	}
	else
	{
		rsb = FB_NEW(*tdbb->getDefaultPool())
			HashJoin(csb, rsbs.getCount(), rsbs.begin(), keys.begin());
	}

	// Activate streams of all the rivers being merged

	for (River** iter = rivers_to_merge.begin(); iter < rivers_to_merge.end(); iter++)
		(*iter)->activate(csb);

	// Pick up any boolean that may apply

	BoolExprNode* boolean = NULL;

	for (tail = opt->opt_conjuncts.begin(); tail < end; tail++)
	{
		BoolExprNode* const node = tail->opt_conjunct_node;

		if (!(tail->opt_conjunct_flags & opt_conjunct_used) &&
			node->computable(csb, -1, false, false))
		{
			compose(tdbb, &boolean, node);
			tail->opt_conjunct_flags |= opt_conjunct_used;
		}
	}

	if (boolean)
		rsb = FB_NEW(*tdbb->getDefaultPool()) FilteredStream(csb, rsb, boolean);

	// Reset all the streams to their original state

	for (stream_nr = 0, fv = flag_vector; stream_nr < csb->csb_n_stream; stream_nr++)
		csb->csb_rpt[stream_nr].csb_flags |= *fv++;

	River* const merged_river = FB_NEW(*tdbb->getDefaultPool()) River(csb, rsb, rivers_to_merge);

	org_rivers.insert(lowest_river_position, merged_river);

	return true;
}


static BoolExprNode* make_inference_node(CompilerScratch* csb, BoolExprNode* boolean,
									jrd_nod* arg1, jrd_nod* arg2)
{
/**************************************
 *
 *	m a k e _ i n f e r e n c e _ n o d e
 *
 **************************************
 *
 * Defined
 *	1996-Jan-15 David Schnepper
 *
 * Functional description
 *	From the predicate, boolean, and infer a new
 *	predicate using arg1 & arg2 as the first two
 *	parameters to the predicate.
 *
 *	This is used when the engine knows A<B and A=C, and
 *	creates a new node to represent the infered knowledge C<B.
 *
 *	Note that this may be sometimes incorrect with 3-value
 *	logic (per Chris Date's Object & Relations seminar).
 *	Later stages of query evaluation evaluate exactly
 *	the originally specified query, so 3-value issues are
 *	caught there.  Making this inference might cause us to
 *	examine more records than needed, but would not result
 *	in incorrect results.
 *
 *	Note that some nodes, specifically nod_like, have
 *	more than two parameters for a boolean operation.
 *	(nod_like has an optional 3rd parameter for the ESCAPE character
 *	 option of SQL)
 *	Nod_sleuth also has an optional 3rd parameter (for the GDML
 *	matching ESCAPE character language).  But nod_sleuth is
 *	(apparently) not considered during optimization.
 *
 *
 **************************************/
	thread_db* tdbb = JRD_get_thread_data();
	DEV_BLKCHK(csb, type_csb);
	DEV_BLKCHK(arg1, type_nod);
	DEV_BLKCHK(arg2, type_nod);

	ComparativeBoolNode* cmpNode = boolean->as<ComparativeBoolNode>();
	fb_assert(cmpNode);	// see our caller

	// Clone the input predicate
	ComparativeBoolNode* newCmpNode = FB_NEW(csb->csb_pool) ComparativeBoolNode(
		csb->csb_pool, cmpNode->blrOp);

	// We may safely copy invariantness flag because
	// (1) we only distribute field equalities
	// (2) invariantness of second argument of STARTING WITH or LIKE is solely
	//    determined by its dependency on any of the fields
	// If provisions above change the line below will have to be modified
	newCmpNode->flags = cmpNode->flags;

	// Share impure area for cached invariant value used to hold pre-compiled
	// pattern for new LIKE and CONTAINING algorithms.
	// Proper cloning of impure area for this node would require careful accounting
	// of new invariant dependencies - we avoid such hassles via using single
	// cached pattern value for all node clones. This is faster too.
	if (newCmpNode->flags & BoolExprNode::FLAG_INVARIANT)
		newCmpNode->impureOffset = cmpNode->impureOffset;

	// But substitute new values for some of the predicate arguments
	newCmpNode->arg1 = CMP_clone_node_opt(tdbb, csb, arg1);
	newCmpNode->arg2 = CMP_clone_node_opt(tdbb, csb, arg2);

	// Arguments after the first two are just cloned (eg: LIKE ESCAPE clause)
	if (cmpNode->arg3)
		newCmpNode->arg3 = CMP_clone_node_opt(tdbb, csb, cmpNode->arg3);

	return newCmpNode;
}


static bool map_equal(const jrd_nod* field1, const jrd_nod* field2, const MapNode* map)
{
/**************************************
 *
 *	m a p _ e q u a l
 *
 **************************************
 *
 * Functional description
 *	Test to see if two fields are equal, where the fields
 *	are in two different streams possibly mapped to each other.
 *	Order of the input fields is important.
 *
 **************************************/
	DEV_BLKCHK(field1, type_nod);
	DEV_BLKCHK(field2, type_nod);

	const FieldNode* fieldNode1 = ExprNode::as<FieldNode>(field1);
	const FieldNode* fieldNode2 = ExprNode::as<FieldNode>(field2);

	if (!fieldNode1 || !fieldNode2)
		return false;

	// look through the mapping and see if we can find an equivalence.
	const NestConst<jrd_nod>* map_ptr = map->items.begin();

	for (const NestConst<jrd_nod>* const map_end = map->items.end(); map_ptr != map_end; ++map_ptr)
	{
		const FieldNode* mapFrom = ExprNode::as<FieldNode>((*map_ptr)->nod_arg[e_asgn_from]);
		const FieldNode* mapTo = ExprNode::as<FieldNode>((*map_ptr)->nod_arg[e_asgn_to]);

		if (!mapFrom || !mapTo)
			continue;

		if (fieldNode1->fieldStream != mapFrom->fieldStream || fieldNode1->fieldId != mapFrom->fieldId)
			continue;

		if (fieldNode2->fieldStream != mapTo->fieldStream || fieldNode2->fieldId != mapTo->fieldId)
			continue;

		return true;
	}

	return false;
}


// Test two field node pointers for symbolic equality.

static bool node_equality(const jrd_nod* node1, const jrd_nod* node2)
{
	DEV_BLKCHK(node1, type_nod);
	DEV_BLKCHK(node2, type_nod);

	if (!node1 || !node2)
		return false;

	if (node1->nod_type != node2->nod_type)
		return false;

	if (node1 == node2)
		return true;

	const FieldNode* fieldNode1 = ExprNode::as<FieldNode>(node1);
	const FieldNode* fieldNode2 = ExprNode::as<FieldNode>(node2);

	if (fieldNode1 && fieldNode2)
	{
		return fieldNode1->fieldStream == fieldNode2->fieldStream &&
			fieldNode1->fieldId == fieldNode2->fieldId;
	}

	return false;
}

static bool node_equality(const BoolExprNode* node1, const BoolExprNode* node2)
{
	DEV_BLKCHK(node1, type_nod);
	DEV_BLKCHK(node2, type_nod);

	if (!node1 || !node2)
		return false;

	if (node1->type != node2->type)
		return false;

	if (node1 == node2)
		return true;

	const ComparativeBoolNode* cmpNode = node1->as<ComparativeBoolNode>();
	const ComparativeBoolNode* cmpNode2 = node2->as<ComparativeBoolNode>();

	if (cmpNode && cmpNode2 && cmpNode->blrOp == cmpNode2->blrOp &&
		(cmpNode->blrOp == blr_eql || cmpNode->blrOp == blr_equiv))
	{
		if (node_equality(cmpNode->arg1, cmpNode2->arg1) &&
			node_equality(cmpNode->arg2, cmpNode2->arg2))
		{
			return true;
		}

		if (node_equality(cmpNode->arg1, cmpNode2->arg2) &&
			node_equality(cmpNode->arg2, cmpNode2->arg1))
		{
			return true;
		}
	}

	return false;
}


static jrd_nod* optimize_like(thread_db* tdbb, CompilerScratch* csb, ComparativeBoolNode* like_node)
{
/**************************************
 *
 *	o p t i m i z e _ l i k e
 *
 **************************************
 *
 * Functional description
 *	Optimize a LIKE expression, if possible,
 *	into a "starting with" AND a "like".  This
 *	will allow us to use the index for the
 *	starting with, and the LIKE can just tag
 *	along for the ride.
 *	But on the ride it does useful work, consider
 *	match LIKE "ab%c".  This is optimized by adding
 *	AND starting_with "ab", but the LIKE clause is
 *	still needed.
 *
 **************************************/
	SET_TDBB(tdbb);

	jrd_nod* match_node = like_node->arg1;
	jrd_nod* pattern_node = like_node->arg2;
	jrd_nod* escape_node = like_node->arg3;

	// if the pattern string or the escape string can't be
	// evaluated at compile time, forget it
	if (!ExprNode::is<LiteralNode>(pattern_node) ||
		(escape_node && !ExprNode::is<LiteralNode>(escape_node)))
	{
		return NULL;
	}

	dsc match_desc;
	CMP_get_desc(tdbb, csb, match_node, &match_desc);

	dsc* pattern_desc = &ExprNode::as<LiteralNode>(pattern_node)->litDesc;
	dsc* escape_desc = 0;
	if (escape_node)
		escape_desc = &ExprNode::as<LiteralNode>(escape_node)->litDesc;

	// if either is not a character expression, forget it
	if ((match_desc.dsc_dtype > dtype_any_text) ||
		(pattern_desc->dsc_dtype > dtype_any_text) ||
		(escape_node && escape_desc->dsc_dtype > dtype_any_text))
	{
		return NULL;
	}

	TextType* matchTextType = INTL_texttype_lookup(tdbb, INTL_TTYPE(&match_desc));
	CharSet* matchCharset = matchTextType->getCharSet();
	TextType* patternTextType = INTL_texttype_lookup(tdbb, INTL_TTYPE(pattern_desc));
	CharSet* patternCharset = patternTextType->getCharSet();

	UCHAR escape_canonic[sizeof(ULONG)];
	UCHAR first_ch[sizeof(ULONG)];
	ULONG first_len;
	UCHAR* p;
	USHORT p_count;

	// Get the escape character, if any
	if (escape_node)
	{
		// Ensure escape string is same character set as match string

		MoveBuffer escape_buffer;

		p_count = MOV_make_string2(tdbb, escape_desc, INTL_TTYPE(&match_desc), &p, escape_buffer);

		first_len = matchCharset->substring(p_count, p, sizeof(first_ch), first_ch, 0, 1);
		matchTextType->canonical(first_len, p, sizeof(escape_canonic), escape_canonic);
	}

	MoveBuffer pattern_buffer;

	p_count = MOV_make_string2(tdbb, pattern_desc, INTL_TTYPE(&match_desc), &p, pattern_buffer);

	first_len = matchCharset->substring(p_count, p, sizeof(first_ch), first_ch, 0, 1);

	UCHAR first_canonic[sizeof(ULONG)];
	matchTextType->canonical(first_len, p, sizeof(first_canonic), first_canonic);

	const BYTE canWidth = matchTextType->getCanonicalWidth();

	// If the first character is a wildcard char, forget it.
	if ((!escape_node ||
			(memcmp(first_canonic, escape_canonic, canWidth) != 0)) &&
		(memcmp(first_canonic, matchTextType->getCanonicalChar(TextType::CHAR_SQL_MATCH_ONE), canWidth) == 0 ||
		 memcmp(first_canonic, matchTextType->getCanonicalChar(TextType::CHAR_SQL_MATCH_ANY), canWidth) == 0))
	{
		return NULL;
	}

	// allocate a literal node to store the starting with string;
	// assume it will be shorter than the pattern string
	// CVC: This assumption may not be true if we use "value like field".

	LiteralNode* literal = FB_NEW(csb->csb_pool) LiteralNode(csb->csb_pool);
	literal->litDesc = *pattern_desc;
	literal->litDesc.dsc_length = 0;

	// Set the string length to point till the first wildcard character.

	HalfStaticArray<UCHAR, BUFFER_SMALL> patternCanonical;
	ULONG patternCanonicalLen = p_count / matchCharset->minBytesPerChar() * canWidth;

	patternCanonicalLen = matchTextType->canonical(p_count, p,
		patternCanonicalLen, patternCanonical.getBuffer(patternCanonicalLen));

	for (UCHAR* patternPtr = patternCanonical.begin(); patternPtr < patternCanonical.end(); )
	{
		// if there are escape characters, skip past them and
		// don't treat the next char as a wildcard
		const UCHAR* patternPtrStart = patternPtr;
		patternPtr += canWidth;

		if (escape_node && (memcmp(patternPtrStart, escape_canonic, canWidth) == 0))
		{
			// Check for Escape character at end of string
			if (!(patternPtr < patternCanonical.end()))
				break;

			patternPtrStart = patternPtr;
			patternPtr += canWidth;
		}
		else if (memcmp(patternPtrStart, matchTextType->getCanonicalChar(TextType::CHAR_SQL_MATCH_ONE), canWidth) == 0 ||
				 memcmp(patternPtrStart, matchTextType->getCanonicalChar(TextType::CHAR_SQL_MATCH_ANY), canWidth) == 0)
		{
			break;
		}

		UCHAR c[sizeof(SLONG)];

		literal->litDesc.dsc_length += patternCharset->substring(pattern_desc->dsc_length,
			pattern_desc->dsc_address, sizeof(c), c,
			(patternPtrStart - patternCanonical.begin()) / canWidth, 1);
	}

	jrd_nod* node = PAR_make_node(tdbb, 1);
	node->nod_type = nod_class_exprnode_jrd;
	node->nod_count = 0;
	node->nod_arg[0] = reinterpret_cast<jrd_nod*>(literal);

	return node;
}


static USHORT river_count(USHORT count, jrd_nod** eq_class)
{
/**************************************
 *
 *	r i v e r _ c o u n t
 *
 **************************************
 *
 * Functional description
 *	Given an sort/merge join equivalence class (vector of node pointers
 *	of representative values for rivers), return the count of rivers
 *	with values.
 *
 **************************************/
	if (*eq_class) {
		DEV_BLKCHK(*eq_class, type_nod);
	}

	USHORT cnt = 0;
	for (USHORT i = 0; i < count; i++, eq_class++)
	{
		if (*eq_class)
		{
			cnt++;
			DEV_BLKCHK(*eq_class, type_nod);
		}
	}

	return cnt;
}


static bool search_stack(const jrd_nod* node, const NodeStack& stack)
{
/**************************************
 *
 *	s e a r c h _ s t a c k
 *
 **************************************
 *
 * Functional description
 *	Search a stack for the presence of a particular value.
 *
 **************************************/
	for (NodeStack::const_iterator iter(stack); iter.hasData(); ++iter)
	{
		if (node_equality(node, iter.object()))
			return true;
	}

	return false;
}


static void set_direction(SortNode* fromClause, SortNode* toClause)
{
/**************************************
 *
 *	s e t _ d i r e c t i o n
 *
 **************************************
 *
 * Functional description
 *	Update the direction of a GROUP BY, DISTINCT, or ORDER BY
 *	clause to the same direction as another clause. Do the same
 *  for the nulls placement flag.
 *
 **************************************/
	const size_t fromCount = fromClause->expressions.getCount();

	fb_assert(fromCount <= toClause->expressions.getCount());
	fb_assert(fromCount == fromClause->descending.getCount() &&
		fromCount == fromClause->nullOrder.getCount());
	fb_assert(toClause->expressions.getCount() == toClause->descending.getCount() &&
		toClause->expressions.getCount() == toClause->nullOrder.getCount());

	for (size_t i = 0; i < fromCount; ++i)
	{
		toClause->descending[i] = fromClause->descending[i];
		toClause->nullOrder[i] = fromClause->nullOrder[i];
	}
}


static void set_position(const SortNode* from_clause, SortNode* to_clause, const MapNode* map)
{
/**************************************
 *
 *	s e t _ p o s i t i o n
 *
 **************************************
 *
 * Functional description
 *	Update the fields in a GROUP BY, DISTINCT, or ORDER BY
 *	clause to the same position as another clause, possibly
 *	using a mapping between the streams.
 *
 **************************************/
	DEV_BLKCHK(from_clause, type_nod);

	// Track the position in the from list with "to_swap", and find the corresponding
	// field in the from list with "to_ptr", then swap the two fields.  By the time
	// we get to the end of the from list, all fields in the to list will be reordered.

	NestConst<jrd_nod>* to_swap = to_clause->expressions.begin();
	const NestConst<jrd_nod>* from_ptr = from_clause->expressions.begin();

	for (const NestConst<jrd_nod>* const from_end = from_clause->expressions.end();
		 from_ptr != from_end; ++from_ptr)
	{
		NestConst<jrd_nod>* to_ptr = to_clause->expressions.begin();

		for (const NestConst<jrd_nod>* const to_end = to_clause->expressions.end();
			 to_ptr != to_end; ++to_ptr)
		{
			const FieldNode* fromField = ExprNode::as<FieldNode>(from_ptr->getObject());
			const FieldNode* toField = ExprNode::as<FieldNode>(to_ptr->getObject());

			if ((map && map_equal(*to_ptr, *from_ptr, map)) ||
				(!map &&
					fromField->fieldStream == toField->fieldStream &&
					fromField->fieldId == toField->fieldId))
			{
				jrd_nod* swap = *to_swap;
				*to_swap = *to_ptr;
				*to_ptr = swap;
			}
		}

		++to_swap;
	}

}


static void set_rse_inactive(CompilerScratch* csb, const RseNode* rse)
{
/***************************************************
 *
 *  s e t _ r s e _ i n a c t i v e
 *
 ***************************************************
 *
 * Functional Description:
 *    Set all the streams involved in an RseNode as inactive. Do it recursively.
 *
 ***************************************************/
	const NestConst<RecordSourceNode>* ptr = rse->rse_relations.begin();

	for (const NestConst<RecordSourceNode>* const end = rse->rse_relations.end(); ptr != end; ++ptr)
	{
		const RecordSourceNode* node = *ptr;

		if (node->type == RseNode::TYPE)
			set_rse_inactive(csb, static_cast<const RseNode*>(node));
		else
		{
			StreamsArray sourceStreams;
			node->getStreams(sourceStreams);

			for (StreamsArray::iterator i = sourceStreams.begin(); i != sourceStreams.end(); ++i)
				csb->csb_rpt[*i].csb_flags &= ~csb_active;
		}
	}
}
