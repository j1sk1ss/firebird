/*
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
 *
 * 2022.02.07 Adriano dos Santos Fernandes: Refactored from dsql.h
 */

#ifndef DSQL_STATEMENTS_H
#define DSQL_STATEMENTS_H

#include "../common/classes/alloc.h"
#include "../common/classes/array.h"
#include "../common/classes/fb_string.h"
#include "../common/classes/NestConst.h"
#include "../common/classes/RefCounted.h"
#include "../jrd/jrd.h"
#include "../jrd/ntrace.h"
#include "../dsql/DsqlRequests.h"

namespace Jrd {


class DdlNode;
class dsql_dbb;
class dsql_msg;
class dsql_par;
class DsqlRequest;
class DsqlCompilerScratch;
class Statement;
class SessionManagementNode;
class TransactionNode;


// Compiled statement - shared by multiple requests.
class DsqlStatement : public Firebird::PermanentStorage
{
public:
	enum Type	// statement type
	{
		TYPE_SELECT, TYPE_SELECT_UPD, TYPE_INSERT, TYPE_DELETE, TYPE_UPDATE, TYPE_UPDATE_CURSOR,
		TYPE_DELETE_CURSOR, TYPE_COMMIT, TYPE_ROLLBACK, TYPE_CREATE_DB, TYPE_DDL, TYPE_START_TRANS,
		TYPE_EXEC_PROCEDURE, TYPE_COMMIT_RETAIN, TYPE_ROLLBACK_RETAIN, TYPE_SET_GENERATOR,
		TYPE_SAVEPOINT, TYPE_EXEC_BLOCK, TYPE_SELECT_BLOCK, TYPE_SESSION_MANAGEMENT,
		TYPE_RETURNING_CURSOR
	};

	// Statement flags.
	static const unsigned FLAG_NO_BATCH		= 0x01;
	static const unsigned FLAG_SELECTABLE	= 0x02;

	static void rethrowDdlException(Firebird::status_exception& ex, bool metadataUpdate, DdlNode* node);

public:
	DsqlStatement(MemoryPool& pool, dsql_dbb* aDsqlAttachment)
		: PermanentStorage(pool),
		  dsqlAttachment(aDsqlAttachment),
		  type(TYPE_SELECT),
		  flags(0),
		  blrVersion(5)
	{
		pool.setStatsGroup(memoryStats);
	}

protected:
	virtual ~DsqlStatement() = default;

public:
	void addRef()
	{
		++refCounter;
	}

	void release();

	bool isCursorBased() const
	{
		switch (type)
		{
			case TYPE_SELECT:
			case TYPE_SELECT_BLOCK:
			case TYPE_SELECT_UPD:
			case TYPE_RETURNING_CURSOR:
				return true;
		}

		return false;
	}

	Type getType() const { return type; }
	void setType(Type value) { type = value; }

	ULONG getFlags() const { return flags; }
	void setFlags(ULONG value) { flags = value; }
	void addFlags(ULONG value) { flags |= value; }

	unsigned getBlrVersion() const { return blrVersion; }
	void setBlrVersion(unsigned value) { blrVersion = value; }

	Firebird::RefStrPtr& getSqlText() { return sqlText; }
	const Firebird::RefStrPtr& getSqlText() const { return sqlText; }
	void setSqlText(Firebird::RefString* value) { sqlText = value; }

	void setOrgText(const char* ptr, ULONG len);
	const Firebird::string& getOrgText() const { return *orgText; }

	dsql_msg* getSendMsg() { return sendMsg; }
	const dsql_msg* getSendMsg() const { return sendMsg; }
	void setSendMsg(dsql_msg* value) { sendMsg = value; }

	dsql_msg* getReceiveMsg() { return receiveMsg; }
	const dsql_msg* getReceiveMsg() const { return receiveMsg; }
	void setReceiveMsg(dsql_msg* value) { receiveMsg = value; }

	Firebird::RefStrPtr getCacheKey() { return cacheKey; }
	void setCacheKey(Firebird::RefStrPtr& value) { cacheKey = value; }
	void resetCacheKey() { cacheKey = nullptr; }

public:
	virtual bool isDml() const
	{
		return false;
	}

	virtual Statement* getStatement() const
	{
		return nullptr;
	}

	virtual bool mustBeReplicated() const
	{
		return false;
	}

	virtual bool shouldPreserveScratch() const
	{
		return true;
	}

	virtual unsigned getSize() const
	{
		return (unsigned) memoryStats.getCurrentUsage();
	}

	virtual void dsqlPass(thread_db* tdbb, DsqlCompilerScratch* scratch, ntrace_result_t* traceResult) = 0;
	virtual DsqlRequest* createRequest(thread_db* tdbb, dsql_dbb* dbb) = 0;

protected:
	virtual void doRelease();

protected:
	dsql_dbb* dsqlAttachment;
	Firebird::MemoryStats memoryStats;
	Type type;					// Type of statement
	ULONG flags;				// generic flag
	unsigned blrVersion;
	Firebird::RefStrPtr sqlText;
	Firebird::RefStrPtr orgText;
	Firebird::RefStrPtr cacheKey;
	dsql_msg* sendMsg = nullptr;				// Message to be sent to start request
	dsql_msg* receiveMsg = nullptr;				// Per record message to be received
	DsqlCompilerScratch* scratch = nullptr;

private:
	Firebird::AtomicCounter refCounter;
};


class DsqlDmlStatement final : public DsqlStatement
{
public:
	MetaName parentCursorName;

	DsqlDmlStatement(MemoryPool& p, dsql_dbb* aDsqlAttachment, StmtNode* aNode)
		: DsqlStatement(p, aDsqlAttachment),
		  parentCursorName(p),
		  node(aNode)
	{
	}

public:
	bool isDml() const override
	{
		return true;
	}

	Statement* getStatement() const override
	{
		return statement;
	}

	bool shouldPreserveScratch() const override
	{
		return false;
	}

	unsigned getSize() const override;

	void dsqlPass(thread_db* tdbb, DsqlCompilerScratch* scratch, ntrace_result_t* traceResult) override;
	DsqlDmlRequest* createRequest(thread_db* tdbb, dsql_dbb* dbb) override;

protected:
	void doRelease() override;

private:
	NestConst<StmtNode> node;
	Statement* statement = nullptr;
};


class DsqlDdlStatement final : public DsqlStatement
{
public:
	DsqlDdlStatement(MemoryPool& p, dsql_dbb* aDsqlAttachment, DdlNode* aNode)
		: DsqlStatement(p, aDsqlAttachment),
		  node(aNode)
	{
	}

	~DsqlDdlStatement();

public:
	bool mustBeReplicated() const override;
	void dsqlPass(thread_db* tdbb, DsqlCompilerScratch* scratch, ntrace_result_t* traceResult) override;
	DsqlDdlRequest* createRequest(thread_db* tdbb, dsql_dbb* dbb) override;

private:
	NestConst<DdlNode> node;
};


class DsqlTransactionStatement final : public DsqlStatement
{
public:
	DsqlTransactionStatement(MemoryPool& p, dsql_dbb* aDsqlAttachment, TransactionNode* aNode)
		: DsqlStatement(p, aDsqlAttachment),
		  node(aNode)
	{
	}

	~DsqlTransactionStatement();

public:
	void dsqlPass(thread_db* tdbb, DsqlCompilerScratch* scratch, ntrace_result_t* traceResult) override;
	DsqlTransactionRequest* createRequest(thread_db* tdbb, dsql_dbb* dbb) override;

private:
	NestConst<TransactionNode> node;
};


class DsqlSessionManagementStatement final : public DsqlStatement
{
public:
	DsqlSessionManagementStatement(MemoryPool& p, dsql_dbb* aDsqlAttachment, SessionManagementNode* aNode)
		: DsqlStatement(p, aDsqlAttachment),
		  node(aNode)
	{
	}

	~DsqlSessionManagementStatement();

public:
	void dsqlPass(thread_db* tdbb, DsqlCompilerScratch* scratch, ntrace_result_t* traceResult) override;
	DsqlSessionManagementRequest* createRequest(thread_db* tdbb, dsql_dbb* dbb) override;

private:
	NestConst<SessionManagementNode> node;
};


}	// namespace Jrd

#endif // DSQL_STATEMENTS_H
