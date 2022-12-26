#pragma once

#include <vector>

// this allows arrays shared by all dynamic meshes; behaves kind of a stack - you can only edit the last slice
template<typename T>
struct VectorSlice
{
	std::vector<T>* vec;
	size_t off;
	size_t len;

	VectorSlice() = default;

	VectorSlice(VectorSlice&) = delete;

	VectorSlice(VectorSlice&& other) : vec(other.vec), off(other.off), len(other.len)
	{
		other.vec = nullptr;
		other.off = other.len = 0;
	}

	VectorSlice(std::vector<T>& vec) : vec(&vec), off(vec.size()), len(0) {}

	void assign_to_end(std::vector<T>& vec_)
	{
		vec = &vec_;
		off = vec_.size();
		len = 0;
	}

	void pop()
	{
		if (vec)
		{
			Assert(off + len == vec->size());
			vec->resize(vec->size() - len);
			vec = nullptr;
			off = len = 0;
		}
	}

	inline std::vector<T>::iterator begin() const
	{
		return vec->begin() + off;
	}

	inline std::vector<T>::iterator end() const
	{
		return vec->begin() + off + len;
	}

	template<class _It>
	inline void add_range(_It first, _It last)
	{
		size_t dist = std::distance(first, last);
		reserve_extra(dist);
		vec->insert(vec->end(), first, last);
		len += dist;
	}

	inline void reserve_extra(size_t n)
	{
		if (vec->capacity() < off + len + n)
			vec->reserve(SmallestPowerOfTwoGreaterOrEqual(off + len + n));
	}

	inline void resize(size_t n)
	{
		vec->resize(off + n);
		len = n;
	}

	template<class _Pr>
	inline void erase_if(_Pr pred)
	{
		auto it = std::remove_if(begin(), end(), pred);
		auto removed = std::distance(it, end());
		vec->erase(it, end());
		len -= removed;
	}

	inline void push_back(const T& elem)
	{
		Assert(off + len == vec->size());
		vec->push_back(elem);
		len++;
	}

	template<class... Params>
	inline void emplace_back(Params&&... params)
	{
		Assert(off + len == vec->size());
		vec->emplace_back(std::forward<Params>(params)...);
		len++;
	}

	inline T& operator[](size_t i) const
	{
		return vec->at(off + i);
	}

	inline bool empty() const
	{
		return len == 0;
	}

	inline size_t size() const
	{
		return len;
	}

	~VectorSlice()
	{
		pop();
	}
};
