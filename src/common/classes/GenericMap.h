/*
 *	PROGRAM:	Client/Server Common Code
 *	MODULE:		StringMap.h
 *	DESCRIPTION:	Secure handling of clumplet buffers
 *
 *  The contents of this file are subject to the Initial
 *  Developer's Public License Version 1.0 (the "License");
 *  you may not use this file except in compliance with the
 *  License. You may obtain a copy of the License at
 *  http://www.ibphoenix.com/main.nfs?a=ibphoenix&page=ibp_idpl.
 *
 *  Software distributed under the License is distributed AS IS,
 *  WITHOUT WARRANTY OF ANY KIND, either express or implied.
 *  See the License for the specific language governing rights
 *  and limitations under the License.
 *
 *  The Original Code was created by Nickolay Samofatov
 *  for the Firebird Open Source RDBMS project.
 *
 *  Copyright (c) 2004 Nickolay Samofatov <nickolay@broadviewsoftware.com>
 *  and all contributors signed below.
 *
 *  All Rights Reserved.
 *  Contributor(s): ______________________________________.
 *
 *
 */

#ifndef STRINGMAP_H
#define STRINGMAP_H

#include "../common/classes/fb_string.h"
#include "../common/classes/fb_pair.h"
#include "../common/classes/rwlock.h"
#include "../common/classes/tree.h"
#include <functional>

namespace Firebird {

//
// Generic map which allows to have POD and non-POD keys and values.
// The class is memory pools friendly.
//
// Usage
//
//   POD key (integer), non-POD value (string):
//     GenericMap<Pair<Right<int, string> > >
//
//   non-POD key (string), POD value (integer):
//     GenericMap<Pair<Left<string, int> > >
//
//   non-POD key (string), non-POD value (string):
//     GenericMap<Pair<Full<string, string> > >
//
template <typename KeyValuePair, typename KeyComparator = DefaultComparator<typename KeyValuePair::first_type> >
class GenericMap : public AutoStorage
{
private:
	template <typename TGenericMap, typename TAccessor, typename TKeyValuePair>
	class BaseIterator
	{
	public:
		BaseIterator(TGenericMap* map, bool initFinished = false)
			: accessor(map),
			  finished(initFinished)
		{
			if (!initFinished)
				finished = !accessor.getFirst();
		}

	public:
		bool operator !=(const BaseIterator& o)
		{
			return !(
				(finished && o.finished) ||
				((!finished && !o.finished && accessor.current() == o.accessor.current())));
		}

		void operator ++()
		{
			fb_assert(!finished);
			finished = !accessor.getNext();
		}

		TKeyValuePair& operator *()
		{
			fb_assert(!finished);
			return *accessor.current();
		}

	private:
		TAccessor accessor;
		bool finished;
	};

public:
	typedef typename KeyValuePair::first_type KeyType;
	typedef typename KeyValuePair::second_type ValueType;

	typedef BePlusTree<KeyValuePair*, KeyType, FirstObjectKey<KeyValuePair>, KeyComparator> ValuesTree;
	typedef typename ValuesTree::Accessor TreeAccessor;
	typedef typename ValuesTree::ConstAccessor ConstTreeAccessor;

	class Accessor
	{
	public:
		explicit Accessor(GenericMap* map) : m_Accessor(&map->tree) {}

		KeyValuePair* current() const { return m_Accessor.current(); }

		bool getFirst() { return m_Accessor.getFirst(); }
		bool getNext() { return m_Accessor.getNext(); }

		bool locate(const KeyType& key) { return m_Accessor.locate(key); }

	private:
		TreeAccessor m_Accessor;
	};

	class ConstAccessor
	{
	public:
		explicit ConstAccessor(const GenericMap* map) : m_Accessor(&map->tree) {}

		const KeyValuePair* current() const { return m_Accessor.current(); }

		bool getFirst() { return m_Accessor.getFirst(); }
		bool getNext() { return m_Accessor.getNext(); }

		bool locate(const KeyType& key) { return m_Accessor.locate(key); }

	private:
		ConstTreeAccessor m_Accessor;
	};

	using Iterator = BaseIterator<GenericMap, Accessor, KeyValuePair>;
	using ConstIterator = BaseIterator<const GenericMap, ConstAccessor, const KeyValuePair>;

	friend class Accessor;
	friend class ConstAccessor;

	GenericMap() : tree(getPool()), mCount(0) { }

	explicit GenericMap(MemoryPool& a_pool)
		: AutoStorage(a_pool), tree(getPool()), mCount(0)
	{ }

	~GenericMap()
	{
		clear();
	}

	void assign(GenericMap& v)
	{
		clear();

		Accessor accessor(&v);

		for (bool found = accessor.getFirst(); found; found = accessor.getNext())
		{
			const KeyValuePair* const current_pair = accessor.current();
			put(current_pair->first, current_pair->second);
		}
	}

	void takeOwnership(GenericMap& from)
	{
		fb_assert(this != &from);

		clear();

		tree = from.tree;
		mCount = from.mCount;

		TreeAccessor treeAccessor(&from.tree);

		if (treeAccessor.getFirst())
		{
			while (true)
			{
				bool haveMore = treeAccessor.fastRemove();
				if (!haveMore)
					break;
			}
		}

		from.mCount = 0;
	}

	// Clear the map
	void clear()
	{
		TreeAccessor treeAccessor(&tree);

		if (treeAccessor.getFirst())
		{
			while (true)
			{
				KeyValuePair* temp = treeAccessor.current();
				bool haveMore = treeAccessor.fastRemove();
				delete temp;
				if (!haveMore)
					break;
			}
		}

		mCount = 0;
	}

	// Returns true if value existed
	bool remove(const KeyType& key)
	{
		TreeAccessor treeAccessor(&tree);

		if (treeAccessor.locate(key))
		{
			KeyValuePair* var = treeAccessor.current();
			treeAccessor.fastRemove();
			delete var;
			mCount--;
			return true;
		}

		return false;
	}

	// Returns true if value existed previously
	bool put(const KeyType& key, const ValueType& value)
	{
		TreeAccessor treeAccessor(&tree);

		if (treeAccessor.locate(key))
		{
			/*
			[PRACTICE_MEMLEAK 09.05]
			Possible memleak. We don`t free second field before re-assign to value.
			Update: Valgrind results, that this is a cache leak. Pointers still point to whole memory.
			*/
			treeAccessor.current()->second = value;
			return true;
		}

		KeyValuePair* var = FB_NEW_POOL(getPool()) KeyValuePair(getPool(), key, value);
		tree.add(var);
		mCount++;
		return false;
	}

	// Returns pointer to the added empty value or null when key already exists
	ValueType* put(const KeyType& key)
	{
		TreeAccessor treeAccessor(&tree);

		if (treeAccessor.locate(key)) {
			return NULL;
		}

		KeyValuePair* var = FB_NEW_POOL(getPool()) KeyValuePair(getPool());
		var->first = key;
		tree.add(var);
		mCount++;
		return &var->second;
	}

	// Returns true if value is found
	bool get(const KeyType& key, ValueType& value) const
	{
		ConstTreeAccessor treeAccessor(&tree);

		if (treeAccessor.locate(key))
		{
			value = treeAccessor.current()->second;
			return true;
		}

		return false;
	}

	// Returns pointer to the found value or null otherwise
	ValueType* get(const KeyType& key) const
	{
		ConstTreeAccessor treeAccessor(&tree);

		if (treeAccessor.locate(key)) {
			return &treeAccessor.current()->second;
		}

		return NULL;
	}

	// If the key is not present, add it. Not synchronized.
	ValueType* getOrPut(const KeyType& key)
	{
		if (auto value = get(key))
			return value;

		return put(key);
	}

	bool exist(const KeyType& key) const
	{
		return ConstTreeAccessor(&tree).locate(key);
	}

	size_t count() const { return mCount; }

	bool isEmpty() const { return mCount == 0; }

	bool hasData() const { return mCount != 0; }

	Accessor accessor()
	{
		return Accessor(this);
	}

	ConstAccessor constAccessor() const
	{
		return ConstAccessor(this);
	}

	Iterator begin()
	{
		return Iterator(this);
	}

	ConstIterator begin() const
	{
		return ConstIterator(this);
	}

	Iterator end()
	{
		return Iterator(this, true);
	}

	ConstIterator end() const
	{
		return ConstIterator(this, true);
	}

	ValueType& compute(RWLock& lock, const KeyType& key, std::function<void (const KeyType&, ValueType&, bool)> func)
	{
		{	// scope
			ReadLockGuard sync(lock, FB_FUNCTION);

			const auto value = get(key);

			if (value)
			{
				func(key, *value, true);
				return *value;
			}
		}

		{	// scope
			WriteLockGuard sync(lock, FB_FUNCTION);

			auto value = get(key);

			if (value)
			{
				func(key, *value, true);
				return *value;
			}

			value = put(key);
			fb_assert(value);

			try
			{
				func(key, *value, false);
			}
			catch (...)
			{
				remove(key);
				throw;
			}

			return *value;
		}
	}

private:
	ValuesTree tree;
	size_t mCount;
};

typedef GenericMap<Pair<Full<string, string> > > StringMap;

template <typename T, typename V, typename KeyComparator = DefaultComparator<T>>
using NonPooledMap = GenericMap<NonPooledPair<T, V>, KeyComparator>;

template <typename T, typename V, typename KeyComparator = DefaultComparator<T>>
using LeftPooledMap = GenericMap<LeftPooledPair<T, V>, KeyComparator>;

template <typename T, typename V, typename KeyComparator = DefaultComparator<T>>
using RightPooledMap = GenericMap<RightPooledPair<T, V>, KeyComparator>;

template <typename T, typename V, typename KeyComparator = DefaultComparator<T>>
using FullPooledMap = GenericMap<FullPooledPair<T, V>, KeyComparator>;

}

#endif
