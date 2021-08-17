#pragma once

// clang-format off

void DrawPortalCollisionFunc();

enum IVP_BOOL {
	IVP_FALSE = 0,
	IVP_TRUE = 1
};


class IVP_Compact_Edge	{
private:
  // for edge navigation
  static int next_table[];
  static int prev_table[];

  // contents
  unsigned int start_point_index:16;		// point index
  signed   int opposite_index:15;		// rel to this // maybe extra array, 3 bits more than tri_index/pierce_index
  unsigned int is_virtual:1;
};


class IVP_Compact_Triangle {
public:
	unsigned int tri_index : 12; // used for upward navigation
	unsigned int pierce_index : 12;
	unsigned int material_index : 7;
	unsigned int is_virtual : 1;

public:
	IVP_Compact_Edge c_three_edges[3];

	inline const IVP_Compact_Edge* get_first_edge() const {
		return &c_three_edges[0];
	};

	inline IVP_Compact_Edge* get_first_edge() {
		return &c_three_edges[0];
	};

	inline const IVP_Compact_Edge* get_edge(int index) const {
		return &c_three_edges[index];
	};

	/*inline const IVP_Compact_Ledge* get_compact_ledge() const {
		const IVP_Compact_Triangle *c_tri = this;
		c_tri -= c_tri->tri_index; // first triangle
		return (IVP_Compact_Ledge*)(((char*)c_tri) - sizeof(IVP_Compact_Ledge));
	}*/

	inline const IVP_Compact_Triangle* get_next_tri() const {
		return this + 1;
	};

	inline IVP_Compact_Triangle* get_next_tri() {
		return this + 1;
	};

	IVP_Compact_Triangle() = default;
};


class IVP_Compact_Ledge {
private:
	int c_point_offset; // byte offset from 'this' to (ledge) point array
	union {
		int ledgetree_node_offset;
		int client_data; // if indicates a non terminal ledge
	};
	unsigned int has_chilren_flag : 2;
	IVP_BOOL is_compact_flag : 2; // if false than compact ledge uses points outside this piece of memory
	unsigned int dummy : 4;
	unsigned int size_div_16 : 24;
	short n_triangles;
	short for_future_use;

public:

	// triangles are always placed behind the class instance
	inline const IVP_Compact_Triangle* get_first_triangle() const {
		return (IVP_Compact_Triangle*)(this + 1);
	};

	inline IVP_Compact_Triangle* get_first_triangle() {
		return (IVP_Compact_Triangle*)(this + 1);
	};
};


/*class CPhysCollideVirtualMesh {
	void GetAllLedges( IVP_U_BigVector<IVP_Compact_Ledge> &ledges ) const {
		const triangleledge_t *pLedges = const_cast<CPhysCollideVirtualMesh *>(this)->AddRef()->GetLedges();
		for ( int i = 0; i < m_ledgeCount; i++ )
		{
			ledges.add( const_cast<IVP_Compact_Ledge *>(&pLedges[i].ledge) );
		}
		const_cast<CPhysCollideVirtualMesh *>(this)->Release();
	}
};*/
