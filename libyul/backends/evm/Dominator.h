/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0
/**
 * Dominator analysis of a control flow graph.
 * The implementation is based on the following paper:
 * https://www.cs.princeton.edu/courses/archive/spr03/cs423/download/dominators.pdf
 * See appendix B pg. 139.
 */
#pragma once

#include <libyul/backends/evm/ControlFlowGraph.h>
#include <libsolutil/Visitor.h>

#include <range/v3/algorithm.hpp>
#include <range/v3/view/drop.hpp>
#include <range/v3/view/reverse.hpp>
#include <range/v3/view/transform.hpp>

#include <deque>
#include <map>
#include <vector>
#include <set>

namespace solidity::yul
{

template<typename Vertex, typename ForEachSuccessor>
class Dominator
{
public:

	Dominator(Vertex const& _entry, size_t _numVertices):
		m_vertices(_numVertices),
		m_immediateDominators(lengauerTarjanDominator(_entry, _numVertices))
	{
		buildDominatorTree();
	}

	std::vector<Vertex> const& vertices() const
	{
		return m_vertices;
	}

	std::map<Vertex, size_t> const& vertexIndices() const
	{
		return m_vertexIndices;
	}

	std::vector<size_t> const& immediateDominators() const
	{
		return m_immediateDominators;
	}

	std::map<size_t, std::vector<size_t>> const& dominatorTree() const
	{
		return m_dominatorTree;
	}

	/// Checks whether ``_a`` dominates ``_b`` by going
	/// through the path from ``_b`` to the entry node.
	/// If ``_a`` is found, then it dominates ``_b``
	/// otherwise it doesn't.
	bool dominates(Vertex const& _a, Vertex const& _b) const
	{
		size_t aIdx = m_vertexIndices[_a];
		size_t bIdx = m_vertexIndices[_b];

		if (aIdx == bIdx)
			return true;

		size_t idomIdx = m_immediateDominators[bIdx];
		while (idomIdx != 0)
		{
			if (idomIdx == aIdx)
				return true;
			idomIdx = m_immediateDominators[idomIdx];
		}
		// Now that we reached the entry node (i.e. idomIdx = 0),
		// either ``aIdx == 0`` or it does not dominate the other node.
		solAssert(idomIdx == 0, "");
		return aIdx == 0;
	}

	/// Find all dominators of a node _v
	/// @note for a vertex ``_v``, the _v’s inclusion in the set of dominators of ``_v`` is implicit.
	std::vector<Vertex> dominatorsOf(Vertex const& _v) const
	{
		solAssert(!m_vertices.empty());
		// The entry node always dominates all other nodes
		std::vector<Vertex> dominators{m_vertices[0]};

		size_t idomIdx = m_immediateDominators[m_vertexIndices[_v]];
		if (idomIdx == 0)
			return dominators;

		while (idomIdx != 0)
		{
			dominators.emplace_back(m_vertices[idomIdx]);
			idomIdx = m_immediateDominators[idomIdx];
		}
		return dominators;
	}

	void buildDominatorTree()
	{
		solAssert(!m_vertices.empty());
		solAssert(!m_immediateDominators.empty());

		//Ignoring the entry node since no one dominates it.
		for (size_t idomIdx: m_immediateDominators | ranges::views::drop(1))
			m_dominatorTree[idomIdx].emplace_back(idomIdx);
	}

	/// Path compression updates the ancestors of vertices along
	/// the path to the ancestor with the minimum label value.
	void compressPath(
		std::vector<size_t> &_ancestor,
		std::vector<size_t> &_label,
		std::vector<size_t> &_semi,
		size_t _v
	) const
	{
		solAssert(_ancestor[_v] != std::numeric_limits<size_t>::max());
		size_t u = _ancestor[_v];
		if (_ancestor[u] != std::numeric_limits<size_t>::max())
		{
			compressPath(_ancestor, _label, _semi, u);
			if (_semi[_label[u]] < _semi[_label[_v]])
				_label[_v] = _label[u];
			_ancestor[_v] = _ancestor[u];
		}
	}

	std::vector<size_t> lengauerTarjanDominator(Vertex const& _entry, size_t numVertices)
	{
		solAssert(numVertices > 0);
		// semi(w): The DFS index of the semidominator of ``w``.
		std::vector<size_t> semi(numVertices, std::numeric_limits<size_t>::max());
		// parent(w): The index of the vertex which is the parent of ``w`` in the spanning
		// tree generated by the DFS.
		std::vector<size_t> parent(numVertices, std::numeric_limits<size_t>::max());
		// ancestor(w): The highest ancestor of a vertex ``w`` in the dominator tree used
		// for path compression.
		std::vector<size_t> ancestor(numVertices, std::numeric_limits<size_t>::max());
		// label(w): The index of the vertex ``w`` with the minimum semidominator in the path
		// to its parent.
		std::vector<size_t> label(numVertices, 0);

		// ``link`` adds an edge to the virtual forest.
		// It copies the parent of w to the ancestor array to limit the search path upwards.
		// TODO: implement sophisticated link-eval algorithm as shown in pg 132
		// See: https://www.cs.princeton.edu/courses/archive/spr03/cs423/download/dominators.pdf
		auto link = [&](size_t _parent, size_t _w)
		{
			ancestor[_w] = _parent;
		};

		// ``eval`` computes the path compression.
		// Finds ancestor with lowest semi-dominator DFS number (i.e. index).
		auto eval = [&](size_t _v) -> size_t
		{
			if (ancestor[_v] != std::numeric_limits<size_t>::max())
			{
				compressPath(ancestor, label, semi, _v);
				return label[_v];
			}
			return _v;
		};

		auto toIdx = [&](Vertex const& v) { return m_vertexIndices[v]; };

		// step 1
		std::set<Vertex> visited;
		// predecessors(w): The set of vertices ``v`` such that (``v``, ``w``) is an edge of the graph.
		std::vector<std::set<size_t>> predecessors(numVertices);
		// bucket(w): a set of vertices whose semidominator is ``w``
		// The index of the array represents the vertex's ``dfIdx``
		std::vector<std::deque<size_t>> bucket(numVertices);
		// idom(w): the index of the immediate dominator of ``w``
		std::vector<size_t> idom(numVertices, std::numeric_limits<size_t>::max());
		// The number of vertices reached during the DFS.
		// The vertices are indexed based on this number.
		size_t dfIdx = 0;
		auto dfs = [&](Vertex const& _v, auto _dfs) -> void {
			if (visited.count(_v))
				return;
			visited.insert(_v);
			m_vertices[dfIdx] = _v;
			m_vertexIndices[_v] = dfIdx;
			semi[dfIdx] = dfIdx;
			label[dfIdx] = dfIdx;
			dfIdx++;
			ForEachSuccessor{}(_v, [&](Vertex const& w) {
				if (semi[dfIdx] == std::numeric_limits<size_t>::max())
				{
					parent[dfIdx] = m_vertexIndices[_v];
					_dfs(w, _dfs);
				}
				predecessors[m_vertexIndices[w]].insert(m_vertexIndices[_v]);
			});
		};
		dfs(_entry, dfs);

		// Process the vertices in decreasing order of the DFS number
		for (size_t w: m_vertices | ranges::views::reverse | ranges::views::transform(toIdx))
		{
			// step 3
			// NOTE: this is an optimization, i.e. performing the step 3 before step 2.
			// The goal is to process the bucket in the beginning of the loop for the vertex ``w``
			// instead of ``parent[w]`` in the end of the loop as described in the original paper.
			// Inverting those steps ensures that a bucket is only processed once and
			// it does not need to be erased.
			// The optimization proposal is available here: https://jgaa.info/accepted/2006/GeorgiadisTarjanWerneck2006.10.1.pdf pg.77
			ranges::for_each(
				bucket[w],
				[&](size_t v)
				{
					size_t u = eval(v);
					idom[v] = (semi[u] < semi[v]) ? u : w;
				}
			);

			// step 2
			for (size_t v: predecessors[w])
			{
				size_t u = eval(v);
				if (semi[u] < semi[w])
					semi[w] = semi[u];
			}
			bucket[semi[w]].emplace_back(w);
			link(parent[w], w);
		}

		// step 4
		idom[0] = 0;
		for (size_t w: m_vertices | ranges::views::drop(1) | ranges::views::transform(toIdx))
			if (idom[w] != semi[w])
				idom[w] = idom[idom[w]];

		return idom;
	}

private:
	/// Keep the list of vertices in the DFS order.
	/// i.e. m_vertices[i] is the vertex whose DFS index is i.
	std::vector<Vertex> m_vertices;

	/// Maps Vertex to its DFS index.
	std::map<Vertex, size_t> m_vertexIndices;

	/// Immediate dominators by index.
	/// Maps a Vertex based on its DFS index (i.e. array index) to its immediate dominator DFS index.
	///
	/// e.g. to get the immediate dominator of a Vertex w:
	/// idomIdx = m_immediateDominators[m_vertexIndices[w]]
	/// idomVertex = m_vertices[domIdx]
	std::vector<size_t> m_immediateDominators;

	/// Maps a Vertex to all vertices that it dominates.
	/// If the vertex does not dominate any other vertex it has no entry in the map.
	std::map<size_t, std::vector<size_t>> m_dominatorTree;
};
}
