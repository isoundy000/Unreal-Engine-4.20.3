// ======================================================================== //
// Copyright 2009-2015 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "bvh4_intersector16_chunk.h"
#include "bvh4_intersector_node.h"

#include "../geometry/triangle4.h"
#include "../geometry/triangle4i.h"
#include "../geometry/triangle4v.h"
#include "../geometry/triangle4v_mb.h"
#include "../geometry/triangle8.h"
#include "../geometry/intersector_iterators.h"
#include "../geometry/bezier1v_intersector.h"
#include "../geometry/bezier1i_intersector.h"
#include "../geometry/triangle_intersector_moeller.h"
#include "../geometry/triangle_intersector_pluecker.h"
#include "../geometry/triangle4i_intersector_pluecker.h"
#include "../geometry/object_intersector16.h"

namespace embree
{
  namespace isa
  {
    template<int types, bool robust, typename PrimitiveIntersector16>
    void BVH4Intersector16Chunk<types, robust, PrimitiveIntersector16>::intersect(int16* valid_i, BVH4* bvh, Ray16& ray)
    {
      /* verify correct input */
      bool16 valid0 = *valid_i == -1; 
#if defined(RTCORE_IGNORE_INVALID_RAYS)
      valid0 &= ray.valid();
#endif
      assert(all(valid0,ray.tnear > -FLT_MIN));
      assert(!(types & BVH4::FLAG_NODE_MB) || all(valid0,ray.time >= 0.0f & ray.time <= 1.0f));
      /* load ray */
      const Vec3f16 rdir = rcp_safe(ray.dir);
      const Vec3f16 org(ray.org), org_rdir = org * rdir;
      float16 ray_tnear = select(valid0,ray.tnear,pos_inf);
      float16 ray_tfar  = select(valid0,ray.tfar ,neg_inf);
      const float16 inf = float16(pos_inf);
      Precalculations pre(valid0,ray);

      /* allocate stack and push root node */
      float16    stack_near[stackSize];
      NodeRef stack_node[stackSize];
      stack_node[0] = BVH4::invalidNode;
      stack_near[0] = inf;
      stack_node[1] = bvh->root;
      stack_near[1] = ray_tnear; 
      NodeRef* stackEnd = stack_node+stackSize;
      NodeRef* __restrict__ sptr_node = stack_node + 2;
      float16*    __restrict__ sptr_near = stack_near + 2;
      
      while (1)
      {
        /* pop next node from stack */
        assert(sptr_node > stack_node);
        sptr_node--;
        sptr_near--;
        NodeRef cur = *sptr_node;
        if (unlikely(cur == BVH4::invalidNode)) {
          assert(sptr_node == stack_node);
          break;
        }
        
        /* cull node if behind closest hit point */
        float16 curDist = *sptr_near;
        if (unlikely(none(ray_tfar > curDist))) 
          continue;
        
        while (1)
        {
          /* process normal nodes */
          if (likely((types & 0x1) && cur.isNode()))
          {
	    const bool16 valid_node = ray_tfar > curDist;
	    STAT3(normal.trav_nodes,1,popcnt(valid_node),16);
	    const Node* __restrict__ const node = cur.node();
	    
	    /* pop of next node */
	    assert(sptr_node > stack_node);
	    sptr_node--;
	    sptr_near--;
	    cur = *sptr_node; 
	    curDist = *sptr_near;
	    
#pragma unroll(4)
	    for (unsigned i=0; i<BVH4::N; i++)
	    {
	      const NodeRef child = node->children[i];
	      if (unlikely(child == BVH4::emptyNode)) break;
	      float16 lnearP; const bool16 lhit = intersect16_node<robust>(node,i,org,rdir,org_rdir,ray_tnear,ray_tfar,lnearP);
	      
	      /* if we hit the child we choose to continue with that child if it 
		 is closer than the current next child, or we push it onto the stack */
	      if (likely(any(lhit)))
	      {
		assert(sptr_node < stackEnd);
		assert(child != BVH4::emptyNode);
		const float16 childDist = select(lhit,lnearP,inf);
		sptr_node++;
		sptr_near++;
		
		/* push cur node onto stack and continue with hit child */
		if (any(childDist < curDist))
		{
		  *(sptr_node-1) = cur;
		  *(sptr_near-1) = curDist; 
		  curDist = childDist;
		  cur = child;
		}
		
		/* push hit child onto stack */
		else {
		  *(sptr_node-1) = child;
		  *(sptr_near-1) = childDist; 
		}
	      }     
	    }
	  }
	  
	  /* process motion blur nodes */
          else if (likely((types & 0x10) && cur.isNodeMB()))
	  {
	    const bool16 valid_node = ray_tfar > curDist;
	    STAT3(normal.trav_nodes,1,popcnt(valid_node),16);
	    const BVH4::NodeMB* __restrict__ const node = cur.nodeMB();
          
	    /* pop of next node */
	    assert(sptr_node > stack_node);
	    sptr_node--;
	    sptr_near--;
	    cur = *sptr_node; 
	    curDist = *sptr_near;
	    
#pragma unroll(4)
	    for (unsigned i=0; i<BVH4::N; i++)
	    {
	      const NodeRef child = node->child(i);
	      if (unlikely(child == BVH4::emptyNode)) break;
	      float16 lnearP; const bool16 lhit = intersect_node(node,i,org,rdir,org_rdir,ray_tnear,ray_tfar,ray.time,lnearP);
	      	      
	      /* if we hit the child we choose to continue with that child if it 
		 is closer than the current next child, or we push it onto the stack */
	      if (likely(any(lhit)))
	      {
		assert(sptr_node < stackEnd);
		assert(child != BVH4::emptyNode);
		const float16 childDist = select(lhit,lnearP,inf);
		sptr_node++;
		sptr_near++;
		
		/* push cur node onto stack and continue with hit child */
		if (any(childDist < curDist))
		{
		  *(sptr_node-1) = cur;
		  *(sptr_near-1) = curDist; 
		  curDist = childDist;
		  cur = child;
		}
		
		/* push hit child onto stack */
		else {
		  *(sptr_node-1) = child;
		  *(sptr_near-1) = childDist; 
		}
	      }	      
	    }
	  }
	  else 
	    break;
	}
        
	/* return if stack is empty */
	if (unlikely(cur == BVH4::invalidNode)) {
	  assert(sptr_node == stack_node);
	  break;
	}
	
	/* intersect leaf */
	assert(cur != BVH4::emptyNode);
	const bool16 valid_leaf = ray_tfar > curDist;
	STAT3(normal.trav_leaves,1,popcnt(valid_leaf),16);
	size_t items; const Primitive* prim = (Primitive*) cur.leaf(items);

        size_t lazy_node = 0;
	PrimitiveIntersector16::intersect(valid_leaf,pre,ray,prim,items,bvh->scene,lazy_node);
	ray_tfar = select(valid_leaf,ray.tfar,ray_tfar);

        if (unlikely(lazy_node)) {
          *sptr_node = lazy_node; sptr_node++;
          *sptr_near = neg_inf;   sptr_near++;
        }
      }
      AVX_ZERO_UPPER();
    }
    
    template<int types, bool robust, typename PrimitiveIntersector16>
    void BVH4Intersector16Chunk<types, robust, PrimitiveIntersector16>::occluded(int16* valid_i, BVH4* bvh, Ray16& ray)
    {
      /* verify correct input */
      bool16 valid = *valid_i == -1;
#if defined(RTCORE_IGNORE_INVALID_RAYS)
      valid &= ray.valid();
#endif
      assert(all(valid,ray.tnear > -FLT_MIN));
      assert(!(types & BVH4::FLAG_NODE_MB) || all(valid,ray.time >= 0.0f & ray.time <= 1.0f));

      /* load ray */
      bool16 terminated = !valid;
      const Vec3f16 rdir = rcp_safe(ray.dir);
      const Vec3f16 org(ray.org), org_rdir = org * rdir;
      float16 ray_tnear = select(valid,ray.tnear,pos_inf);
      float16 ray_tfar  = select(valid,ray.tfar ,neg_inf);
      const float16 inf = float16(pos_inf);
      Precalculations pre(valid,ray);

      /* allocate stack and push root node */
      float16    stack_near[stackSize];
      NodeRef stack_node[stackSize];
      stack_node[0] = BVH4::invalidNode;
      stack_near[0] = inf;
      stack_node[1] = bvh->root;
      stack_near[1] = ray_tnear; 
      NodeRef* stackEnd = stack_node+stackSize;
      NodeRef* __restrict__ sptr_node = stack_node + 2;
      float16*    __restrict__ sptr_near = stack_near + 2;
      
      while (1)
      {
        /* pop next node from stack */
        assert(sptr_node > stack_node);
        sptr_node--;
        sptr_near--;
        NodeRef cur = *sptr_node;
        if (unlikely(cur == BVH4::invalidNode)) {
          assert(sptr_node == stack_node);
          break;
        }
        
        /* cull node if behind closest hit point */
        float16 curDist = *sptr_near;
        if (unlikely(none(ray_tfar > curDist))) 
          continue;
        
        while (1)
        {
	  /* process normal nodes */
          if (likely((types & 0x1) && cur.isNode()))
          {
	    const bool16 valid_node = ray_tfar > curDist;
	    STAT3(normal.trav_nodes,1,popcnt(valid_node),16);
	    const Node* __restrict__ const node = cur.node();
	    
	    /* pop of next node */
	    assert(sptr_node > stack_node);
	    sptr_node--;
	    sptr_near--;
	    cur = *sptr_node; 
	    curDist = *sptr_near;
	    
#pragma unroll(4)
	    for (unsigned i=0; i<BVH4::N; i++)
	    {
	      const NodeRef child = node->children[i];
	      if (unlikely(child == BVH4::emptyNode)) break;
	      float16 lnearP; const bool16 lhit = intersect16_node<robust>(node,i,org,rdir,org_rdir,ray_tnear,ray_tfar,lnearP);
	      
	      /* if we hit the child we choose to continue with that child if it 
		 is closer than the current next child, or we push it onto the stack */
	      if (likely(any(lhit)))
	      {
		assert(sptr_node < stackEnd);
		assert(child != BVH4::emptyNode);
		const float16 childDist = select(lhit,lnearP,inf);
		sptr_node++;
		sptr_near++;

		/* push cur node onto stack and continue with hit child */
		if (any(childDist < curDist))
		{
		  *(sptr_node-1) = cur;
		  *(sptr_near-1) = curDist; 
		  curDist = childDist;
		  cur = child;
		}
		
		/* push hit child onto stack */
		else {
		  *(sptr_node-1) = child;
		  *(sptr_near-1) = childDist; 
		}
	      }     
	    }
	  }
	  
	  /* process motion blur nodes */
          else if (likely((types & 0x10) && cur.isNodeMB()))
	  {
	    const bool16 valid_node = ray_tfar > curDist;
	    STAT3(normal.trav_nodes,1,popcnt(valid_node),16);
	    const BVH4::NodeMB* __restrict__ const node = cur.nodeMB();
          
	    /* pop of next node */
	    assert(sptr_node > stack_node);
	    sptr_node--;
	    sptr_near--;
	    cur = *sptr_node; 
	    curDist = *sptr_near;
	    
#pragma unroll(4)
	    for (unsigned i=0; i<BVH4::N; i++)
	    {
	      const NodeRef child = node->child(i);
	      if (unlikely(child == BVH4::emptyNode)) break;
	      float16 lnearP; const bool16 lhit = intersect_node(node,i,org,rdir,org_rdir,ray_tnear,ray_tfar,ray.time,lnearP);
	      	      
	      /* if we hit the child we choose to continue with that child if it 
		 is closer than the current next child, or we push it onto the stack */
	      if (likely(any(lhit)))
	      {
		assert(sptr_node < stackEnd);
		assert(child != BVH4::emptyNode);
		const float16 childDist = select(lhit,lnearP,inf);
		sptr_node++;
		sptr_near++;

		/* push cur node onto stack and continue with hit child */
		if (any(childDist < curDist))
		{
		  *(sptr_node-1) = cur;
		  *(sptr_near-1) = curDist; 
		  curDist = childDist;
		  cur = child;
		}
		
		/* push hit child onto stack */
		else {
		  *(sptr_node-1) = child;
		  *(sptr_near-1) = childDist; 
		}
	      }	      
	    }
	  }
	  else 
	    break;
	}
        
        /* return if stack is empty */
        if (unlikely(cur == BVH4::invalidNode)) {
          assert(sptr_node == stack_node);
          break;
        }
        
        /* intersect leaf */
	assert(cur != BVH4::emptyNode);
        const bool16 valid_leaf = ray_tfar > curDist;
        STAT3(shadow.trav_leaves,1,popcnt(valid_leaf),16);
        size_t items; const Primitive* prim = (Primitive*) cur.leaf(items);

        size_t lazy_node = 0;
        terminated |= valid_leaf & PrimitiveIntersector16::occluded(valid_leaf,pre,ray,prim,items,bvh->scene,lazy_node);
        if (all(terminated)) break;
        ray_tfar = select(terminated,neg_inf,ray_tfar);

        if (unlikely(lazy_node)) {
          *sptr_node = lazy_node; sptr_node++;
          *sptr_near = neg_inf;   sptr_near++;
        }
      }
      store16i(valid & terminated,&ray.geomID,0);
      AVX_ZERO_UPPER();
    }

    DEFINE_INTERSECTOR16(BVH4Bezier1vIntersector16Chunk, BVH4Intersector16Chunk<0x1 COMMA false COMMA ArrayIntersector16<Bezier1vIntersectorN<Ray16> > >);
    DEFINE_INTERSECTOR16(BVH4Bezier1iIntersector16Chunk, BVH4Intersector16Chunk<0x1 COMMA false COMMA ArrayIntersector16<Bezier1iIntersectorN<Ray16> > >);
    DEFINE_INTERSECTOR16(BVH4Triangle4Intersector16ChunkMoeller, BVH4Intersector16Chunk<0x1 COMMA false COMMA ArrayIntersector16<TriangleNIntersectorMMoellerTrumbore<Ray16 COMMA Triangle4 COMMA true> > >);
    DEFINE_INTERSECTOR16(BVH4Triangle4Intersector16ChunkMoellerNoFilter, BVH4Intersector16Chunk<0x1 COMMA false COMMA ArrayIntersector16<TriangleNIntersectorMMoellerTrumbore<Ray16 COMMA Triangle4 COMMA false> > >);
    DEFINE_INTERSECTOR16(BVH4Triangle8Intersector16ChunkMoeller, BVH4Intersector16Chunk<0x1 COMMA false COMMA ArrayIntersector16<TriangleNIntersectorMMoellerTrumbore<Ray16 COMMA Triangle8 COMMA true> > >);
    DEFINE_INTERSECTOR16(BVH4Triangle8Intersector16ChunkMoellerNoFilter, BVH4Intersector16Chunk<0x1 COMMA false COMMA ArrayIntersector16<TriangleNIntersectorMMoellerTrumbore<Ray16 COMMA Triangle8 COMMA false> > >);
    DEFINE_INTERSECTOR16(BVH4Triangle4vIntersector16ChunkPluecker, BVH4Intersector16Chunk<0x1 COMMA true COMMA ArrayIntersector16<TriangleNvIntersectorMPluecker<Ray16 COMMA Triangle4v COMMA true> > >);
    DEFINE_INTERSECTOR16(BVH4Triangle4iIntersector16ChunkPluecker, BVH4Intersector16Chunk<0x1 COMMA true COMMA ArrayIntersector16<Triangle4iIntersectorMPluecker<Ray16 COMMA true> > >);
    DEFINE_INTERSECTOR16(BVH4VirtualIntersector16Chunk, BVH4Intersector16Chunk<0x1 COMMA false COMMA ArrayIntersector16<ObjectIntersector16> >);
    DEFINE_INTERSECTOR16(BVH4Triangle4vMBIntersector16ChunkMoeller, BVH4Intersector16Chunk<0x10 COMMA false COMMA ArrayIntersector16<TriangleNMblurIntersectorMMoellerTrumbore<Ray16 COMMA Triangle4vMB COMMA true> > >);
  }
}
