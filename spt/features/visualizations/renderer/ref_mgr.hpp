#pragma once

/*
* The SDK has a class like this for managing an IRefCounted, but materials and dwrite pointers aren't that :/.
* The REF_MGR class needs to be default constructable and have const Release()/AddRef() methods that take in type
* T as a param, note that I don't check for null here. TODO I probably should
*/
template<class T, typename REF_MGR, REF_MGR refMgr = REF_MGR{}>
struct AutoRefPtr
{
	AutoRefPtr() : _ptr(nullptr) {}

	// This doesn't add a ref since ptr must already have a ref to exist, but it does mean that you don't want to
	// create two instances of this from the same ptr; using the copy ctor is fine since that'll add a ref.
	AutoRefPtr(const T _ptr) : _ptr(_ptr) {}

	AutoRefPtr(const AutoRefPtr& other) : _ptr(other._ptr)
	{
		refMgr.AddRef(_ptr);
	}

	AutoRefPtr(AutoRefPtr&& rhs) : _ptr(rhs._ptr)
	{
		rhs._ptr = nullptr;
	}

	AutoRefPtr& operator=(T ptr)
	{
		if (_ptr != ptr)
		{
			refMgr.AddRef(ptr);
			refMgr.Release(_ptr);
			_ptr = ptr;
		}
		return *this;
	}

	AutoRefPtr& operator=(const AutoRefPtr rhs)
	{
		return *this = rhs._ptr;
	}

	operator T() const
	{
		return _ptr;
	}

	T operator->() const
	{
		return _ptr;
	}

	void Release()
	{
		refMgr.Release(_ptr);
	}

	~AutoRefPtr()
	{
		Release();
	}

	T _ptr;
};

// meant to be used with the IUnknown interface
template<class T>
struct IUnknownRefMgr
{
	inline void AddRef(T ptr) const
	{
		if (ptr)
			ptr->AddRef();
	}

	inline void Release(T& ptr) const
	{
		if (ptr)
			ptr->Release();
		ptr = nullptr;
	}
};

template<class T>
using IUnknownRef = AutoRefPtr<T, IUnknownRefMgr<T>>;
