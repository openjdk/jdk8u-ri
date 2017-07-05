/*
 * Copyright 1998-2009 Sun Microsystems, Inc.  All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 */

#include "incls/_precompiled.incl"
#include "incls/_loopnode.cpp.incl"

//=============================================================================
//------------------------------is_loop_iv-------------------------------------
// Determine if a node is Counted loop induction variable.
// The method is declared in node.hpp.
const Node* Node::is_loop_iv() const {
  if (this->is_Phi() && !this->as_Phi()->is_copy() &&
      this->as_Phi()->region()->is_CountedLoop() &&
      this->as_Phi()->region()->as_CountedLoop()->phi() == this) {
    return this;
  } else {
    return NULL;
  }
}

//=============================================================================
//------------------------------dump_spec--------------------------------------
// Dump special per-node info
#ifndef PRODUCT
void LoopNode::dump_spec(outputStream *st) const {
  if( is_inner_loop () ) st->print( "inner " );
  if( is_partial_peel_loop () ) st->print( "partial_peel " );
  if( partial_peel_has_failed () ) st->print( "partial_peel_failed " );
}
#endif

//------------------------------get_early_ctrl---------------------------------
// Compute earliest legal control
Node *PhaseIdealLoop::get_early_ctrl( Node *n ) {
  assert( !n->is_Phi() && !n->is_CFG(), "this code only handles data nodes" );
  uint i;
  Node *early;
  if( n->in(0) ) {
    early = n->in(0);
    if( !early->is_CFG() ) // Might be a non-CFG multi-def
      early = get_ctrl(early);        // So treat input as a straight data input
    i = 1;
  } else {
    early = get_ctrl(n->in(1));
    i = 2;
  }
  uint e_d = dom_depth(early);
  assert( early, "" );
  for( ; i < n->req(); i++ ) {
    Node *cin = get_ctrl(n->in(i));
    assert( cin, "" );
    // Keep deepest dominator depth
    uint c_d = dom_depth(cin);
    if( c_d > e_d ) {           // Deeper guy?
      early = cin;              // Keep deepest found so far
      e_d = c_d;
    } else if( c_d == e_d &&    // Same depth?
               early != cin ) { // If not equal, must use slower algorithm
      // If same depth but not equal, one _must_ dominate the other
      // and we want the deeper (i.e., dominated) guy.
      Node *n1 = early;
      Node *n2 = cin;
      while( 1 ) {
        n1 = idom(n1);          // Walk up until break cycle
        n2 = idom(n2);
        if( n1 == cin ||        // Walked early up to cin
            dom_depth(n2) < c_d )
          break;                // early is deeper; keep him
        if( n2 == early ||      // Walked cin up to early
            dom_depth(n1) < c_d ) {
          early = cin;          // cin is deeper; keep him
          break;
        }
      }
      e_d = dom_depth(early);   // Reset depth register cache
    }
  }

  // Return earliest legal location
  assert(early == find_non_split_ctrl(early), "unexpected early control");

  return early;
}

//------------------------------set_early_ctrl---------------------------------
// Set earliest legal control
void PhaseIdealLoop::set_early_ctrl( Node *n ) {
  Node *early = get_early_ctrl(n);

  // Record earliest legal location
  set_ctrl(n, early);
}

//------------------------------set_subtree_ctrl-------------------------------
// set missing _ctrl entries on new nodes
void PhaseIdealLoop::set_subtree_ctrl( Node *n ) {
  // Already set?  Get out.
  if( _nodes[n->_idx] ) return;
  // Recursively set _nodes array to indicate where the Node goes
  uint i;
  for( i = 0; i < n->req(); ++i ) {
    Node *m = n->in(i);
    if( m && m != C->root() )
      set_subtree_ctrl( m );
  }

  // Fixup self
  set_early_ctrl( n );
}

//------------------------------is_counted_loop--------------------------------
Node *PhaseIdealLoop::is_counted_loop( Node *x, IdealLoopTree *loop ) {
  PhaseGVN *gvn = &_igvn;

  // Counted loop head must be a good RegionNode with only 3 not NULL
  // control input edges: Self, Entry, LoopBack.
  if ( x->in(LoopNode::Self) == NULL || x->req() != 3 )
    return NULL;

  Node *init_control = x->in(LoopNode::EntryControl);
  Node *back_control = x->in(LoopNode::LoopBackControl);
  if( init_control == NULL || back_control == NULL )    // Partially dead
    return NULL;
  // Must also check for TOP when looking for a dead loop
  if( init_control->is_top() || back_control->is_top() )
    return NULL;

  // Allow funny placement of Safepoint
  if( back_control->Opcode() == Op_SafePoint )
    back_control = back_control->in(TypeFunc::Control);

  // Controlling test for loop
  Node *iftrue = back_control;
  uint iftrue_op = iftrue->Opcode();
  if( iftrue_op != Op_IfTrue &&
      iftrue_op != Op_IfFalse )
    // I have a weird back-control.  Probably the loop-exit test is in
    // the middle of the loop and I am looking at some trailing control-flow
    // merge point.  To fix this I would have to partially peel the loop.
    return NULL; // Obscure back-control

  // Get boolean guarding loop-back test
  Node *iff = iftrue->in(0);
  if( get_loop(iff) != loop || !iff->in(1)->is_Bool() ) return NULL;
  BoolNode *test = iff->in(1)->as_Bool();
  BoolTest::mask bt = test->_test._test;
  float cl_prob = iff->as_If()->_prob;
  if( iftrue_op == Op_IfFalse ) {
    bt = BoolTest(bt).negate();
    cl_prob = 1.0 - cl_prob;
  }
  // Get backedge compare
  Node *cmp = test->in(1);
  int cmp_op = cmp->Opcode();
  if( cmp_op != Op_CmpI )
    return NULL;                // Avoid pointer & float compares

  // Find the trip-counter increment & limit.  Limit must be loop invariant.
  Node *incr  = cmp->in(1);
  Node *limit = cmp->in(2);

  // ---------
  // need 'loop()' test to tell if limit is loop invariant
  // ---------

  if( !is_member( loop, get_ctrl(incr) ) ) { // Swapped trip counter and limit?
    Node *tmp = incr;           // Then reverse order into the CmpI
    incr = limit;
    limit = tmp;
    bt = BoolTest(bt).commute(); // And commute the exit test
  }
  if( is_member( loop, get_ctrl(limit) ) ) // Limit must loop-invariant
    return NULL;

  // Trip-counter increment must be commutative & associative.
  uint incr_op = incr->Opcode();
  if( incr_op == Op_Phi && incr->req() == 3 ) {
    incr = incr->in(2);         // Assume incr is on backedge of Phi
    incr_op = incr->Opcode();
  }
  Node* trunc1 = NULL;
  Node* trunc2 = NULL;
  const TypeInt* iv_trunc_t = NULL;
  if (!(incr = CountedLoopNode::match_incr_with_optional_truncation(incr, &trunc1, &trunc2, &iv_trunc_t))) {
    return NULL; // Funny increment opcode
  }

  // Get merge point
  Node *xphi = incr->in(1);
  Node *stride = incr->in(2);
  if( !stride->is_Con() ) {     // Oops, swap these
    if( !xphi->is_Con() )       // Is the other guy a constant?
      return NULL;              // Nope, unknown stride, bail out
    Node *tmp = xphi;           // 'incr' is commutative, so ok to swap
    xphi = stride;
    stride = tmp;
  }
  //if( loop(xphi) != l) return NULL;// Merge point is in inner loop??
  if( !xphi->is_Phi() ) return NULL; // Too much math on the trip counter
  PhiNode *phi = xphi->as_Phi();

  // Stride must be constant
  const Type *stride_t = stride->bottom_type();
  int stride_con = stride_t->is_int()->get_con();
  assert( stride_con, "missed some peephole opt" );

  // Phi must be of loop header; backedge must wrap to increment
  if( phi->region() != x ) return NULL;
  if( trunc1 == NULL && phi->in(LoopNode::LoopBackControl) != incr ||
      trunc1 != NULL && phi->in(LoopNode::LoopBackControl) != trunc1 ) {
    return NULL;
  }
  Node *init_trip = phi->in(LoopNode::EntryControl);
  //if (!init_trip->is_Con()) return NULL; // avoid rolling over MAXINT/MININT

  // If iv trunc type is smaller than int, check for possible wrap.
  if (!TypeInt::INT->higher_equal(iv_trunc_t)) {
    assert(trunc1 != NULL, "must have found some truncation");

    // Get a better type for the phi (filtered thru if's)
    const TypeInt* phi_ft = filtered_type(phi);

    // Can iv take on a value that will wrap?
    //
    // Ensure iv's limit is not within "stride" of the wrap value.
    //
    // Example for "short" type
    //    Truncation ensures value is in the range -32768..32767 (iv_trunc_t)
    //    If the stride is +10, then the last value of the induction
    //    variable before the increment (phi_ft->_hi) must be
    //    <= 32767 - 10 and (phi_ft->_lo) must be >= -32768 to
    //    ensure no truncation occurs after the increment.

    if (stride_con > 0) {
      if (iv_trunc_t->_hi - phi_ft->_hi < stride_con ||
          iv_trunc_t->_lo > phi_ft->_lo) {
        return NULL;  // truncation may occur
      }
    } else if (stride_con < 0) {
      if (iv_trunc_t->_lo - phi_ft->_lo > stride_con ||
          iv_trunc_t->_hi < phi_ft->_hi) {
        return NULL;  // truncation may occur
      }
    }
    // No possibility of wrap so truncation can be discarded
    // Promote iv type to Int
  } else {
    assert(trunc1 == NULL && trunc2 == NULL, "no truncation for int");
  }

  // =================================================
  // ---- SUCCESS!   Found A Trip-Counted Loop!  -----
  //
  // Canonicalize the condition on the test.  If we can exactly determine
  // the trip-counter exit value, then set limit to that value and use
  // a '!=' test.  Otherwise use condition '<' for count-up loops and
  // '>' for count-down loops.  If the condition is inverted and we will
  // be rolling through MININT to MAXINT, then bail out.

  C->print_method("Before CountedLoop", 3);

  // Check for SafePoint on backedge and remove
  Node *sfpt = x->in(LoopNode::LoopBackControl);
  if( sfpt->Opcode() == Op_SafePoint && is_deleteable_safept(sfpt)) {
    lazy_replace( sfpt, iftrue );
    loop->_tail = iftrue;
  }


  // If compare points to incr, we are ok.  Otherwise the compare
  // can directly point to the phi; in this case adjust the compare so that
  // it points to the incr by adjusting the limit.
  if( cmp->in(1) == phi || cmp->in(2) == phi )
    limit = gvn->transform(new (C, 3) AddINode(limit,stride));

  // trip-count for +-tive stride should be: (limit - init_trip + stride - 1)/stride.
  // Final value for iterator should be: trip_count * stride + init_trip.
  const Type *limit_t = limit->bottom_type();
  const Type *init_t = init_trip->bottom_type();
  Node *one_p = gvn->intcon( 1);
  Node *one_m = gvn->intcon(-1);

  Node *trip_count = NULL;
  Node *hook = new (C, 6) Node(6);
  switch( bt ) {
  case BoolTest::eq:
    return NULL;                // Bail out, but this loop trips at most twice!
  case BoolTest::ne:            // Ahh, the case we desire
    if( stride_con == 1 )
      trip_count = gvn->transform(new (C, 3) SubINode(limit,init_trip));
    else if( stride_con == -1 )
      trip_count = gvn->transform(new (C, 3) SubINode(init_trip,limit));
    else
      return NULL;              // Odd stride; must prove we hit limit exactly
    set_subtree_ctrl( trip_count );
    //_loop.map(trip_count->_idx,loop(limit));
    break;
  case BoolTest::le:            // Maybe convert to '<' case
    limit = gvn->transform(new (C, 3) AddINode(limit,one_p));
    set_subtree_ctrl( limit );
    hook->init_req(4, limit);

    bt = BoolTest::lt;
    // Make the new limit be in the same loop nest as the old limit
    //_loop.map(limit->_idx,limit_loop);
    // Fall into next case
  case BoolTest::lt: {          // Maybe convert to '!=' case
    if( stride_con < 0 ) return NULL; // Count down loop rolls through MAXINT
    Node *range = gvn->transform(new (C, 3) SubINode(limit,init_trip));
    set_subtree_ctrl( range );
    hook->init_req(0, range);

    Node *bias  = gvn->transform(new (C, 3) AddINode(range,stride));
    set_subtree_ctrl( bias );
    hook->init_req(1, bias);

    Node *bias1 = gvn->transform(new (C, 3) AddINode(bias,one_m));
    set_subtree_ctrl( bias1 );
    hook->init_req(2, bias1);

    trip_count  = gvn->transform(new (C, 3) DivINode(0,bias1,stride));
    set_subtree_ctrl( trip_count );
    hook->init_req(3, trip_count);
    break;
  }

  case BoolTest::ge:            // Maybe convert to '>' case
    limit = gvn->transform(new (C, 3) AddINode(limit,one_m));
    set_subtree_ctrl( limit );
    hook->init_req(4 ,limit);

    bt = BoolTest::gt;
    // Make the new limit be in the same loop nest as the old limit
    //_loop.map(limit->_idx,limit_loop);
    // Fall into next case
  case BoolTest::gt: {          // Maybe convert to '!=' case
    if( stride_con > 0 ) return NULL; // count up loop rolls through MININT
    Node *range = gvn->transform(new (C, 3) SubINode(limit,init_trip));
    set_subtree_ctrl( range );
    hook->init_req(0, range);

    Node *bias  = gvn->transform(new (C, 3) AddINode(range,stride));
    set_subtree_ctrl( bias );
    hook->init_req(1, bias);

    Node *bias1 = gvn->transform(new (C, 3) AddINode(bias,one_p));
    set_subtree_ctrl( bias1 );
    hook->init_req(2, bias1);

    trip_count  = gvn->transform(new (C, 3) DivINode(0,bias1,stride));
    set_subtree_ctrl( trip_count );
    hook->init_req(3, trip_count);
    break;
  }
  }

  Node *span = gvn->transform(new (C, 3) MulINode(trip_count,stride));
  set_subtree_ctrl( span );
  hook->init_req(5, span);

  limit = gvn->transform(new (C, 3) AddINode(span,init_trip));
  set_subtree_ctrl( limit );

  // Build a canonical trip test.
  // Clone code, as old values may be in use.
  incr = incr->clone();
  incr->set_req(1,phi);
  incr->set_req(2,stride);
  incr = _igvn.register_new_node_with_optimizer(incr);
  set_early_ctrl( incr );
  _igvn.hash_delete(phi);
  phi->set_req_X( LoopNode::LoopBackControl, incr, &_igvn );

  // If phi type is more restrictive than Int, raise to
  // Int to prevent (almost) infinite recursion in igvn
  // which can only handle integer types for constants or minint..maxint.
  if (!TypeInt::INT->higher_equal(phi->bottom_type())) {
    Node* nphi = PhiNode::make(phi->in(0), phi->in(LoopNode::EntryControl), TypeInt::INT);
    nphi->set_req(LoopNode::LoopBackControl, phi->in(LoopNode::LoopBackControl));
    nphi = _igvn.register_new_node_with_optimizer(nphi);
    set_ctrl(nphi, get_ctrl(phi));
    _igvn.subsume_node(phi, nphi);
    phi = nphi->as_Phi();
  }
  cmp = cmp->clone();
  cmp->set_req(1,incr);
  cmp->set_req(2,limit);
  cmp = _igvn.register_new_node_with_optimizer(cmp);
  set_ctrl(cmp, iff->in(0));

  Node *tmp = test->clone();
  assert( tmp->is_Bool(), "" );
  test = (BoolNode*)tmp;
  (*(BoolTest*)&test->_test)._test = bt; //BoolTest::ne;
  test->set_req(1,cmp);
  _igvn.register_new_node_with_optimizer(test);
  set_ctrl(test, iff->in(0));
  // If the exit test is dead, STOP!
  if( test == NULL ) return NULL;
  _igvn.hash_delete(iff);
  iff->set_req_X( 1, test, &_igvn );

  // Replace the old IfNode with a new LoopEndNode
  Node *lex = _igvn.register_new_node_with_optimizer(new (C, 2) CountedLoopEndNode( iff->in(0), iff->in(1), cl_prob, iff->as_If()->_fcnt ));
  IfNode *le = lex->as_If();
  uint dd = dom_depth(iff);
  set_idom(le, le->in(0), dd); // Update dominance for loop exit
  set_loop(le, loop);

  // Get the loop-exit control
  Node *if_f = iff->as_If()->proj_out(!(iftrue_op == Op_IfTrue));

  // Need to swap loop-exit and loop-back control?
  if( iftrue_op == Op_IfFalse ) {
    Node *ift2=_igvn.register_new_node_with_optimizer(new (C, 1) IfTrueNode (le));
    Node *iff2=_igvn.register_new_node_with_optimizer(new (C, 1) IfFalseNode(le));

    loop->_tail = back_control = ift2;
    set_loop(ift2, loop);
    set_loop(iff2, get_loop(if_f));

    // Lazy update of 'get_ctrl' mechanism.
    lazy_replace_proj( if_f  , iff2 );
    lazy_replace_proj( iftrue, ift2 );

    // Swap names
    if_f   = iff2;
    iftrue = ift2;
  } else {
    _igvn.hash_delete(if_f  );
    _igvn.hash_delete(iftrue);
    if_f  ->set_req_X( 0, le, &_igvn );
    iftrue->set_req_X( 0, le, &_igvn );
  }

  set_idom(iftrue, le, dd+1);
  set_idom(if_f,   le, dd+1);

  // Now setup a new CountedLoopNode to replace the existing LoopNode
  CountedLoopNode *l = new (C, 3) CountedLoopNode(init_control, back_control);
  // The following assert is approximately true, and defines the intention
  // of can_be_counted_loop.  It fails, however, because phase->type
  // is not yet initialized for this loop and its parts.
  //assert(l->can_be_counted_loop(this), "sanity");
  _igvn.register_new_node_with_optimizer(l);
  set_loop(l, loop);
  loop->_head = l;
  // Fix all data nodes placed at the old loop head.
  // Uses the lazy-update mechanism of 'get_ctrl'.
  lazy_replace( x, l );
  set_idom(l, init_control, dom_depth(x));

  // Check for immediately preceding SafePoint and remove
  Node *sfpt2 = le->in(0);
  if( sfpt2->Opcode() == Op_SafePoint && is_deleteable_safept(sfpt2))
    lazy_replace( sfpt2, sfpt2->in(TypeFunc::Control));

  // Free up intermediate goo
  _igvn.remove_dead_node(hook);

  C->print_method("After CountedLoop", 3);

  // Return trip counter
  return trip_count;
}


//------------------------------Ideal------------------------------------------
// Return a node which is more "ideal" than the current node.
// Attempt to convert into a counted-loop.
Node *LoopNode::Ideal(PhaseGVN *phase, bool can_reshape) {
  if (!can_be_counted_loop(phase)) {
    phase->C->set_major_progress();
  }
  return RegionNode::Ideal(phase, can_reshape);
}


//=============================================================================
//------------------------------Ideal------------------------------------------
// Return a node which is more "ideal" than the current node.
// Attempt to convert into a counted-loop.
Node *CountedLoopNode::Ideal(PhaseGVN *phase, bool can_reshape) {
  return RegionNode::Ideal(phase, can_reshape);
}

//------------------------------dump_spec--------------------------------------
// Dump special per-node info
#ifndef PRODUCT
void CountedLoopNode::dump_spec(outputStream *st) const {
  LoopNode::dump_spec(st);
  if( stride_is_con() ) {
    st->print("stride: %d ",stride_con());
  } else {
    st->print("stride: not constant ");
  }
  if( is_pre_loop () ) st->print("pre of N%d" , _main_idx );
  if( is_main_loop() ) st->print("main of N%d", _idx );
  if( is_post_loop() ) st->print("post of N%d", _main_idx );
}
#endif

//=============================================================================
int CountedLoopEndNode::stride_con() const {
  return stride()->bottom_type()->is_int()->get_con();
}


//----------------------match_incr_with_optional_truncation--------------------
// Match increment with optional truncation:
// CHAR: (i+1)&0x7fff, BYTE: ((i+1)<<8)>>8, or SHORT: ((i+1)<<16)>>16
// Return NULL for failure. Success returns the increment node.
Node* CountedLoopNode::match_incr_with_optional_truncation(
                      Node* expr, Node** trunc1, Node** trunc2, const TypeInt** trunc_type) {
  // Quick cutouts:
  if (expr == NULL || expr->req() != 3)  return false;

  Node *t1 = NULL;
  Node *t2 = NULL;
  const TypeInt* trunc_t = TypeInt::INT;
  Node* n1 = expr;
  int   n1op = n1->Opcode();

  // Try to strip (n1 & M) or (n1 << N >> N) from n1.
  if (n1op == Op_AndI &&
      n1->in(2)->is_Con() &&
      n1->in(2)->bottom_type()->is_int()->get_con() == 0x7fff) {
    // %%% This check should match any mask of 2**K-1.
    t1 = n1;
    n1 = t1->in(1);
    n1op = n1->Opcode();
    trunc_t = TypeInt::CHAR;
  } else if (n1op == Op_RShiftI &&
             n1->in(1) != NULL &&
             n1->in(1)->Opcode() == Op_LShiftI &&
             n1->in(2) == n1->in(1)->in(2) &&
             n1->in(2)->is_Con()) {
    jint shift = n1->in(2)->bottom_type()->is_int()->get_con();
    // %%% This check should match any shift in [1..31].
    if (shift == 16 || shift == 8) {
      t1 = n1;
      t2 = t1->in(1);
      n1 = t2->in(1);
      n1op = n1->Opcode();
      if (shift == 16) {
        trunc_t = TypeInt::SHORT;
      } else if (shift == 8) {
        trunc_t = TypeInt::BYTE;
      }
    }
  }

  // If (maybe after stripping) it is an AddI, we won:
  if (n1op == Op_AddI) {
    *trunc1 = t1;
    *trunc2 = t2;
    *trunc_type = trunc_t;
    return n1;
  }

  // failed
  return NULL;
}


//------------------------------filtered_type--------------------------------
// Return a type based on condition control flow
// A successful return will be a type that is restricted due
// to a series of dominating if-tests, such as:
//    if (i < 10) {
//       if (i > 0) {
//          here: "i" type is [1..10)
//       }
//    }
// or a control flow merge
//    if (i < 10) {
//       do {
//          phi( , ) -- at top of loop type is [min_int..10)
//         i = ?
//       } while ( i < 10)
//
const TypeInt* PhaseIdealLoop::filtered_type( Node *n, Node* n_ctrl) {
  assert(n && n->bottom_type()->is_int(), "must be int");
  const TypeInt* filtered_t = NULL;
  if (!n->is_Phi()) {
    assert(n_ctrl != NULL || n_ctrl == C->top(), "valid control");
    filtered_t = filtered_type_from_dominators(n, n_ctrl);

  } else {
    Node* phi    = n->as_Phi();
    Node* region = phi->in(0);
    assert(n_ctrl == NULL || n_ctrl == region, "ctrl parameter must be region");
    if (region && region != C->top()) {
      for (uint i = 1; i < phi->req(); i++) {
        Node* val   = phi->in(i);
        Node* use_c = region->in(i);
        const TypeInt* val_t = filtered_type_from_dominators(val, use_c);
        if (val_t != NULL) {
          if (filtered_t == NULL) {
            filtered_t = val_t;
          } else {
            filtered_t = filtered_t->meet(val_t)->is_int();
          }
        }
      }
    }
  }
  const TypeInt* n_t = _igvn.type(n)->is_int();
  if (filtered_t != NULL) {
    n_t = n_t->join(filtered_t)->is_int();
  }
  return n_t;
}


//------------------------------filtered_type_from_dominators--------------------------------
// Return a possibly more restrictive type for val based on condition control flow of dominators
const TypeInt* PhaseIdealLoop::filtered_type_from_dominators( Node* val, Node *use_ctrl) {
  if (val->is_Con()) {
     return val->bottom_type()->is_int();
  }
  uint if_limit = 10; // Max number of dominating if's visited
  const TypeInt* rtn_t = NULL;

  if (use_ctrl && use_ctrl != C->top()) {
    Node* val_ctrl = get_ctrl(val);
    uint val_dom_depth = dom_depth(val_ctrl);
    Node* pred = use_ctrl;
    uint if_cnt = 0;
    while (if_cnt < if_limit) {
      if ((pred->Opcode() == Op_IfTrue || pred->Opcode() == Op_IfFalse)) {
        if_cnt++;
        const TypeInt* if_t = IfNode::filtered_int_type(&_igvn, val, pred);
        if (if_t != NULL) {
          if (rtn_t == NULL) {
            rtn_t = if_t;
          } else {
            rtn_t = rtn_t->join(if_t)->is_int();
          }
        }
      }
      pred = idom(pred);
      if (pred == NULL || pred == C->top()) {
        break;
      }
      // Stop if going beyond definition block of val
      if (dom_depth(pred) < val_dom_depth) {
        break;
      }
    }
  }
  return rtn_t;
}


//------------------------------dump_spec--------------------------------------
// Dump special per-node info
#ifndef PRODUCT
void CountedLoopEndNode::dump_spec(outputStream *st) const {
  if( in(TestValue)->is_Bool() ) {
    BoolTest bt( test_trip()); // Added this for g++.

    st->print("[");
    bt.dump_on(st);
    st->print("]");
  }
  st->print(" ");
  IfNode::dump_spec(st);
}
#endif

//=============================================================================
//------------------------------is_member--------------------------------------
// Is 'l' a member of 'this'?
int IdealLoopTree::is_member( const IdealLoopTree *l ) const {
  while( l->_nest > _nest ) l = l->_parent;
  return l == this;
}

//------------------------------set_nest---------------------------------------
// Set loop tree nesting depth.  Accumulate _has_call bits.
int IdealLoopTree::set_nest( uint depth ) {
  _nest = depth;
  int bits = _has_call;
  if( _child ) bits |= _child->set_nest(depth+1);
  if( bits ) _has_call = 1;
  if( _next  ) bits |= _next ->set_nest(depth  );
  return bits;
}

//------------------------------split_fall_in----------------------------------
// Split out multiple fall-in edges from the loop header.  Move them to a
// private RegionNode before the loop.  This becomes the loop landing pad.
void IdealLoopTree::split_fall_in( PhaseIdealLoop *phase, int fall_in_cnt ) {
  PhaseIterGVN &igvn = phase->_igvn;
  uint i;

  // Make a new RegionNode to be the landing pad.
  Node *landing_pad = new (phase->C, fall_in_cnt+1) RegionNode( fall_in_cnt+1 );
  phase->set_loop(landing_pad,_parent);
  // Gather all the fall-in control paths into the landing pad
  uint icnt = fall_in_cnt;
  uint oreq = _head->req();
  for( i = oreq-1; i>0; i-- )
    if( !phase->is_member( this, _head->in(i) ) )
      landing_pad->set_req(icnt--,_head->in(i));

  // Peel off PhiNode edges as well
  for (DUIterator_Fast jmax, j = _head->fast_outs(jmax); j < jmax; j++) {
    Node *oj = _head->fast_out(j);
    if( oj->is_Phi() ) {
      PhiNode* old_phi = oj->as_Phi();
      assert( old_phi->region() == _head, "" );
      igvn.hash_delete(old_phi);   // Yank from hash before hacking edges
      Node *p = PhiNode::make_blank(landing_pad, old_phi);
      uint icnt = fall_in_cnt;
      for( i = oreq-1; i>0; i-- ) {
        if( !phase->is_member( this, _head->in(i) ) ) {
          p->init_req(icnt--, old_phi->in(i));
          // Go ahead and clean out old edges from old phi
          old_phi->del_req(i);
        }
      }
      // Search for CSE's here, because ZKM.jar does a lot of
      // loop hackery and we need to be a little incremental
      // with the CSE to avoid O(N^2) node blow-up.
      Node *p2 = igvn.hash_find_insert(p); // Look for a CSE
      if( p2 ) {                // Found CSE
        p->destruct();          // Recover useless new node
        p = p2;                 // Use old node
      } else {
        igvn.register_new_node_with_optimizer(p, old_phi);
      }
      // Make old Phi refer to new Phi.
      old_phi->add_req(p);
      // Check for the special case of making the old phi useless and
      // disappear it.  In JavaGrande I have a case where this useless
      // Phi is the loop limit and prevents recognizing a CountedLoop
      // which in turn prevents removing an empty loop.
      Node *id_old_phi = old_phi->Identity( &igvn );
      if( id_old_phi != old_phi ) { // Found a simple identity?
        // Note that I cannot call 'subsume_node' here, because
        // that will yank the edge from old_phi to the Region and
        // I'm mid-iteration over the Region's uses.
        for (DUIterator_Last imin, i = old_phi->last_outs(imin); i >= imin; ) {
          Node* use = old_phi->last_out(i);
          igvn.hash_delete(use);
          igvn._worklist.push(use);
          uint uses_found = 0;
          for (uint j = 0; j < use->len(); j++) {
            if (use->in(j) == old_phi) {
              if (j < use->req()) use->set_req (j, id_old_phi);
              else                use->set_prec(j, id_old_phi);
              uses_found++;
            }
          }
          i -= uses_found;    // we deleted 1 or more copies of this edge
        }
      }
      igvn._worklist.push(old_phi);
    }
  }
  // Finally clean out the fall-in edges from the RegionNode
  for( i = oreq-1; i>0; i-- ) {
    if( !phase->is_member( this, _head->in(i) ) ) {
      _head->del_req(i);
    }
  }
  // Transform landing pad
  igvn.register_new_node_with_optimizer(landing_pad, _head);
  // Insert landing pad into the header
  _head->add_req(landing_pad);
}

//------------------------------split_outer_loop-------------------------------
// Split out the outermost loop from this shared header.
void IdealLoopTree::split_outer_loop( PhaseIdealLoop *phase ) {
  PhaseIterGVN &igvn = phase->_igvn;

  // Find index of outermost loop; it should also be my tail.
  uint outer_idx = 1;
  while( _head->in(outer_idx) != _tail ) outer_idx++;

  // Make a LoopNode for the outermost loop.
  Node *ctl = _head->in(LoopNode::EntryControl);
  Node *outer = new (phase->C, 3) LoopNode( ctl, _head->in(outer_idx) );
  outer = igvn.register_new_node_with_optimizer(outer, _head);
  phase->set_created_loop_node();
  // Outermost loop falls into '_head' loop
  _head->set_req(LoopNode::EntryControl, outer);
  _head->del_req(outer_idx);
  // Split all the Phis up between '_head' loop and 'outer' loop.
  for (DUIterator_Fast jmax, j = _head->fast_outs(jmax); j < jmax; j++) {
    Node *out = _head->fast_out(j);
    if( out->is_Phi() ) {
      PhiNode *old_phi = out->as_Phi();
      assert( old_phi->region() == _head, "" );
      Node *phi = PhiNode::make_blank(outer, old_phi);
      phi->init_req(LoopNode::EntryControl,    old_phi->in(LoopNode::EntryControl));
      phi->init_req(LoopNode::LoopBackControl, old_phi->in(outer_idx));
      phi = igvn.register_new_node_with_optimizer(phi, old_phi);
      // Make old Phi point to new Phi on the fall-in path
      igvn.hash_delete(old_phi);
      old_phi->set_req(LoopNode::EntryControl, phi);
      old_phi->del_req(outer_idx);
      igvn._worklist.push(old_phi);
    }
  }

  // Use the new loop head instead of the old shared one
  _head = outer;
  phase->set_loop(_head, this);
}

//------------------------------fix_parent-------------------------------------
static void fix_parent( IdealLoopTree *loop, IdealLoopTree *parent ) {
  loop->_parent = parent;
  if( loop->_child ) fix_parent( loop->_child, loop   );
  if( loop->_next  ) fix_parent( loop->_next , parent );
}

//------------------------------estimate_path_freq-----------------------------
static float estimate_path_freq( Node *n ) {
  // Try to extract some path frequency info
  IfNode *iff;
  for( int i = 0; i < 50; i++ ) { // Skip through a bunch of uncommon tests
    uint nop = n->Opcode();
    if( nop == Op_SafePoint ) {   // Skip any safepoint
      n = n->in(0);
      continue;
    }
    if( nop == Op_CatchProj ) {   // Get count from a prior call
      // Assume call does not always throw exceptions: means the call-site
      // count is also the frequency of the fall-through path.
      assert( n->is_CatchProj(), "" );
      if( ((CatchProjNode*)n)->_con != CatchProjNode::fall_through_index )
        return 0.0f;            // Assume call exception path is rare
      Node *call = n->in(0)->in(0)->in(0);
      assert( call->is_Call(), "expect a call here" );
      const JVMState *jvms = ((CallNode*)call)->jvms();
      ciMethodData* methodData = jvms->method()->method_data();
      if (!methodData->is_mature())  return 0.0f; // No call-site data
      ciProfileData* data = methodData->bci_to_data(jvms->bci());
      if ((data == NULL) || !data->is_CounterData()) {
        // no call profile available, try call's control input
        n = n->in(0);
        continue;
      }
      return data->as_CounterData()->count()/FreqCountInvocations;
    }
    // See if there's a gating IF test
    Node *n_c = n->in(0);
    if( !n_c->is_If() ) break;       // No estimate available
    iff = n_c->as_If();
    if( iff->_fcnt != COUNT_UNKNOWN )   // Have a valid count?
      // Compute how much count comes on this path
      return ((nop == Op_IfTrue) ? iff->_prob : 1.0f - iff->_prob) * iff->_fcnt;
    // Have no count info.  Skip dull uncommon-trap like branches.
    if( (nop == Op_IfTrue  && iff->_prob < PROB_LIKELY_MAG(5)) ||
        (nop == Op_IfFalse && iff->_prob > PROB_UNLIKELY_MAG(5)) )
      break;
    // Skip through never-taken branch; look for a real loop exit.
    n = iff->in(0);
  }
  return 0.0f;                  // No estimate available
}

//------------------------------merge_many_backedges---------------------------
// Merge all the backedges from the shared header into a private Region.
// Feed that region as the one backedge to this loop.
void IdealLoopTree::merge_many_backedges( PhaseIdealLoop *phase ) {
  uint i;

  // Scan for the top 2 hottest backedges
  float hotcnt = 0.0f;
  float warmcnt = 0.0f;
  uint hot_idx = 0;
  // Loop starts at 2 because slot 1 is the fall-in path
  for( i = 2; i < _head->req(); i++ ) {
    float cnt = estimate_path_freq(_head->in(i));
    if( cnt > hotcnt ) {       // Grab hottest path
      warmcnt = hotcnt;
      hotcnt = cnt;
      hot_idx = i;
    } else if( cnt > warmcnt ) { // And 2nd hottest path
      warmcnt = cnt;
    }
  }

  // See if the hottest backedge is worthy of being an inner loop
  // by being much hotter than the next hottest backedge.
  if( hotcnt <= 0.0001 ||
      hotcnt < 2.0*warmcnt ) hot_idx = 0;// No hot backedge

  // Peel out the backedges into a private merge point; peel
  // them all except optionally hot_idx.
  PhaseIterGVN &igvn = phase->_igvn;

  Node *hot_tail = NULL;
  // Make a Region for the merge point
  Node *r = new (phase->C, 1) RegionNode(1);
  for( i = 2; i < _head->req(); i++ ) {
    if( i != hot_idx )
      r->add_req( _head->in(i) );
    else hot_tail = _head->in(i);
  }
  igvn.register_new_node_with_optimizer(r, _head);
  // Plug region into end of loop _head, followed by hot_tail
  while( _head->req() > 3 ) _head->del_req( _head->req()-1 );
  _head->set_req(2, r);
  if( hot_idx ) _head->add_req(hot_tail);

  // Split all the Phis up between '_head' loop and the Region 'r'
  for (DUIterator_Fast jmax, j = _head->fast_outs(jmax); j < jmax; j++) {
    Node *out = _head->fast_out(j);
    if( out->is_Phi() ) {
      PhiNode* n = out->as_Phi();
      igvn.hash_delete(n);      // Delete from hash before hacking edges
      Node *hot_phi = NULL;
      Node *phi = new (phase->C, r->req()) PhiNode(r, n->type(), n->adr_type());
      // Check all inputs for the ones to peel out
      uint j = 1;
      for( uint i = 2; i < n->req(); i++ ) {
        if( i != hot_idx )
          phi->set_req( j++, n->in(i) );
        else hot_phi = n->in(i);
      }
      // Register the phi but do not transform until whole place transforms
      igvn.register_new_node_with_optimizer(phi, n);
      // Add the merge phi to the old Phi
      while( n->req() > 3 ) n->del_req( n->req()-1 );
      n->set_req(2, phi);
      if( hot_idx ) n->add_req(hot_phi);
    }
  }


  // Insert a new IdealLoopTree inserted below me.  Turn it into a clone
  // of self loop tree.  Turn self into a loop headed by _head and with
  // tail being the new merge point.
  IdealLoopTree *ilt = new IdealLoopTree( phase, _head, _tail );
  phase->set_loop(_tail,ilt);   // Adjust tail
  _tail = r;                    // Self's tail is new merge point
  phase->set_loop(r,this);
  ilt->_child = _child;         // New guy has my children
  _child = ilt;                 // Self has new guy as only child
  ilt->_parent = this;          // new guy has self for parent
  ilt->_nest = _nest;           // Same nesting depth (for now)

  // Starting with 'ilt', look for child loop trees using the same shared
  // header.  Flatten these out; they will no longer be loops in the end.
  IdealLoopTree **pilt = &_child;
  while( ilt ) {
    if( ilt->_head == _head ) {
      uint i;
      for( i = 2; i < _head->req(); i++ )
        if( _head->in(i) == ilt->_tail )
          break;                // Still a loop
      if( i == _head->req() ) { // No longer a loop
        // Flatten ilt.  Hang ilt's "_next" list from the end of
        // ilt's '_child' list.  Move the ilt's _child up to replace ilt.
        IdealLoopTree **cp = &ilt->_child;
        while( *cp ) cp = &(*cp)->_next;   // Find end of child list
        *cp = ilt->_next;       // Hang next list at end of child list
        *pilt = ilt->_child;    // Move child up to replace ilt
        ilt->_head = NULL;      // Flag as a loop UNIONED into parent
        ilt = ilt->_child;      // Repeat using new ilt
        continue;               // do not advance over ilt->_child
      }
      assert( ilt->_tail == hot_tail, "expected to only find the hot inner loop here" );
      phase->set_loop(_head,ilt);
    }
    pilt = &ilt->_child;        // Advance to next
    ilt = *pilt;
  }

  if( _child ) fix_parent( _child, this );
}

//------------------------------beautify_loops---------------------------------
// Split shared headers and insert loop landing pads.
// Insert a LoopNode to replace the RegionNode.
// Return TRUE if loop tree is structurally changed.
bool IdealLoopTree::beautify_loops( PhaseIdealLoop *phase ) {
  bool result = false;
  // Cache parts in locals for easy
  PhaseIterGVN &igvn = phase->_igvn;

  phase->C->print_method("Before beautify loops", 3);

  igvn.hash_delete(_head);      // Yank from hash before hacking edges

  // Check for multiple fall-in paths.  Peel off a landing pad if need be.
  int fall_in_cnt = 0;
  for( uint i = 1; i < _head->req(); i++ )
    if( !phase->is_member( this, _head->in(i) ) )
      fall_in_cnt++;
  assert( fall_in_cnt, "at least 1 fall-in path" );
  if( fall_in_cnt > 1 )         // Need a loop landing pad to merge fall-ins
    split_fall_in( phase, fall_in_cnt );

  // Swap inputs to the _head and all Phis to move the fall-in edge to
  // the left.
  fall_in_cnt = 1;
  while( phase->is_member( this, _head->in(fall_in_cnt) ) )
    fall_in_cnt++;
  if( fall_in_cnt > 1 ) {
    // Since I am just swapping inputs I do not need to update def-use info
    Node *tmp = _head->in(1);
    _head->set_req( 1, _head->in(fall_in_cnt) );
    _head->set_req( fall_in_cnt, tmp );
    // Swap also all Phis
    for (DUIterator_Fast imax, i = _head->fast_outs(imax); i < imax; i++) {
      Node* phi = _head->fast_out(i);
      if( phi->is_Phi() ) {
        igvn.hash_delete(phi); // Yank from hash before hacking edges
        tmp = phi->in(1);
        phi->set_req( 1, phi->in(fall_in_cnt) );
        phi->set_req( fall_in_cnt, tmp );
      }
    }
  }
  assert( !phase->is_member( this, _head->in(1) ), "left edge is fall-in" );
  assert(  phase->is_member( this, _head->in(2) ), "right edge is loop" );

  // If I am a shared header (multiple backedges), peel off the many
  // backedges into a private merge point and use the merge point as
  // the one true backedge.
  if( _head->req() > 3 ) {
    // Merge the many backedges into a single backedge.
    merge_many_backedges( phase );
    result = true;
  }

  // If I am a shared header (multiple backedges), peel off myself loop.
  // I better be the outermost loop.
  if( _head->req() > 3 ) {
    split_outer_loop( phase );
    result = true;

  } else if( !_head->is_Loop() && !_irreducible ) {
    // Make a new LoopNode to replace the old loop head
    Node *l = new (phase->C, 3) LoopNode( _head->in(1), _head->in(2) );
    l = igvn.register_new_node_with_optimizer(l, _head);
    phase->set_created_loop_node();
    // Go ahead and replace _head
    phase->_igvn.subsume_node( _head, l );
    _head = l;
    phase->set_loop(_head, this);
    for (DUIterator_Fast imax, i = l->fast_outs(imax); i < imax; i++)
      phase->_igvn.add_users_to_worklist(l->fast_out(i));
  }

  // Now recursively beautify nested loops
  if( _child ) result |= _child->beautify_loops( phase );
  if( _next  ) result |= _next ->beautify_loops( phase );
  return result;
}

//------------------------------allpaths_check_safepts----------------------------
// Allpaths backwards scan from loop tail, terminating each path at first safepoint
// encountered.  Helper for check_safepts.
void IdealLoopTree::allpaths_check_safepts(VectorSet &visited, Node_List &stack) {
  assert(stack.size() == 0, "empty stack");
  stack.push(_tail);
  visited.Clear();
  visited.set(_tail->_idx);
  while (stack.size() > 0) {
    Node* n = stack.pop();
    if (n->is_Call() && n->as_Call()->guaranteed_safepoint()) {
      // Terminate this path
    } else if (n->Opcode() == Op_SafePoint) {
      if (_phase->get_loop(n) != this) {
        if (_required_safept == NULL) _required_safept = new Node_List();
        _required_safept->push(n);  // save the one closest to the tail
      }
      // Terminate this path
    } else {
      uint start = n->is_Region() ? 1 : 0;
      uint end   = n->is_Region() && !n->is_Loop() ? n->req() : start + 1;
      for (uint i = start; i < end; i++) {
        Node* in = n->in(i);
        assert(in->is_CFG(), "must be");
        if (!visited.test_set(in->_idx) && is_member(_phase->get_loop(in))) {
          stack.push(in);
        }
      }
    }
  }
}

//------------------------------check_safepts----------------------------
// Given dominators, try to find loops with calls that must always be
// executed (call dominates loop tail).  These loops do not need non-call
// safepoints (ncsfpt).
//
// A complication is that a safepoint in a inner loop may be needed
// by an outer loop. In the following, the inner loop sees it has a
// call (block 3) on every path from the head (block 2) to the
// backedge (arc 3->2).  So it deletes the ncsfpt (non-call safepoint)
// in block 2, _but_ this leaves the outer loop without a safepoint.
//
//          entry  0
//                 |
//                 v
// outer 1,2    +->1
//              |  |
//              |  v
//              |  2<---+  ncsfpt in 2
//              |_/|\   |
//                 | v  |
// inner 2,3      /  3  |  call in 3
//               /   |  |
//              v    +--+
//        exit  4
//
//
// This method creates a list (_required_safept) of ncsfpt nodes that must
// be protected is created for each loop. When a ncsfpt maybe deleted, it
// is first looked for in the lists for the outer loops of the current loop.
//
// The insights into the problem:
//  A) counted loops are okay
//  B) innermost loops are okay (only an inner loop can delete
//     a ncsfpt needed by an outer loop)
//  C) a loop is immune from an inner loop deleting a safepoint
//     if the loop has a call on the idom-path
//  D) a loop is also immune if it has a ncsfpt (non-call safepoint) on the
//     idom-path that is not in a nested loop
//  E) otherwise, an ncsfpt on the idom-path that is nested in an inner
//     loop needs to be prevented from deletion by an inner loop
//
// There are two analyses:
//  1) The first, and cheaper one, scans the loop body from
//     tail to head following the idom (immediate dominator)
//     chain, looking for the cases (C,D,E) above.
//     Since inner loops are scanned before outer loops, there is summary
//     information about inner loops.  Inner loops can be skipped over
//     when the tail of an inner loop is encountered.
//
//  2) The second, invoked if the first fails to find a call or ncsfpt on
//     the idom path (which is rare), scans all predecessor control paths
//     from the tail to the head, terminating a path when a call or sfpt
//     is encountered, to find the ncsfpt's that are closest to the tail.
//
void IdealLoopTree::check_safepts(VectorSet &visited, Node_List &stack) {
  // Bottom up traversal
  IdealLoopTree* ch = _child;
  while (ch != NULL) {
    ch->check_safepts(visited, stack);
    ch = ch->_next;
  }

  if (!_head->is_CountedLoop() && !_has_sfpt && _parent != NULL && !_irreducible) {
    bool  has_call         = false; // call on dom-path
    bool  has_local_ncsfpt = false; // ncsfpt on dom-path at this loop depth
    Node* nonlocal_ncsfpt  = NULL;  // ncsfpt on dom-path at a deeper depth
    // Scan the dom-path nodes from tail to head
    for (Node* n = tail(); n != _head; n = _phase->idom(n)) {
      if (n->is_Call() && n->as_Call()->guaranteed_safepoint()) {
        has_call = true;
        _has_sfpt = 1;          // Then no need for a safept!
        break;
      } else if (n->Opcode() == Op_SafePoint) {
        if (_phase->get_loop(n) == this) {
          has_local_ncsfpt = true;
          break;
        }
        if (nonlocal_ncsfpt == NULL) {
          nonlocal_ncsfpt = n; // save the one closest to the tail
        }
      } else {
        IdealLoopTree* nlpt = _phase->get_loop(n);
        if (this != nlpt) {
          // If at an inner loop tail, see if the inner loop has already
          // recorded seeing a call on the dom-path (and stop.)  If not,
          // jump to the head of the inner loop.
          assert(is_member(nlpt), "nested loop");
          Node* tail = nlpt->_tail;
          if (tail->in(0)->is_If()) tail = tail->in(0);
          if (n == tail) {
            // If inner loop has call on dom-path, so does outer loop
            if (nlpt->_has_sfpt) {
              has_call = true;
              _has_sfpt = 1;
              break;
            }
            // Skip to head of inner loop
            assert(_phase->is_dominator(_head, nlpt->_head), "inner head dominated by outer head");
            n = nlpt->_head;
          }
        }
      }
    }
    // Record safept's that this loop needs preserved when an
    // inner loop attempts to delete it's safepoints.
    if (_child != NULL && !has_call && !has_local_ncsfpt) {
      if (nonlocal_ncsfpt != NULL) {
        if (_required_safept == NULL) _required_safept = new Node_List();
        _required_safept->push(nonlocal_ncsfpt);
      } else {
        // Failed to find a suitable safept on the dom-path.  Now use
        // an all paths walk from tail to head, looking for safepoints to preserve.
        allpaths_check_safepts(visited, stack);
      }
    }
  }
}

//---------------------------is_deleteable_safept----------------------------
// Is safept not required by an outer loop?
bool PhaseIdealLoop::is_deleteable_safept(Node* sfpt) {
  assert(sfpt->Opcode() == Op_SafePoint, "");
  IdealLoopTree* lp = get_loop(sfpt)->_parent;
  while (lp != NULL) {
    Node_List* sfpts = lp->_required_safept;
    if (sfpts != NULL) {
      for (uint i = 0; i < sfpts->size(); i++) {
        if (sfpt == sfpts->at(i))
          return false;
      }
    }
    lp = lp->_parent;
  }
  return true;
}

//------------------------------counted_loop-----------------------------------
// Convert to counted loops where possible
void IdealLoopTree::counted_loop( PhaseIdealLoop *phase ) {

  // For grins, set the inner-loop flag here
  if( !_child ) {
    if( _head->is_Loop() ) _head->as_Loop()->set_inner_loop();
  }

  if( _head->is_CountedLoop() ||
      phase->is_counted_loop( _head, this ) ) {
    _has_sfpt = 1;              // Indicate we do not need a safepoint here

    // Look for a safepoint to remove
    for (Node* n = tail(); n != _head; n = phase->idom(n))
      if (n->Opcode() == Op_SafePoint && phase->get_loop(n) == this &&
          phase->is_deleteable_safept(n))
        phase->lazy_replace(n,n->in(TypeFunc::Control));

    CountedLoopNode *cl = _head->as_CountedLoop();
    Node *incr = cl->incr();
    if( !incr ) return;         // Dead loop?
    Node *init = cl->init_trip();
    Node *phi  = cl->phi();
    // protect against stride not being a constant
    if( !cl->stride_is_con() ) return;
    int stride_con = cl->stride_con();

    // Look for induction variables

    // Visit all children, looking for Phis
    for (DUIterator i = cl->outs(); cl->has_out(i); i++) {
      Node *out = cl->out(i);
      // Look for other phis (secondary IVs). Skip dead ones
      if (!out->is_Phi() || out == phi || !phase->has_node(out)) continue;
      PhiNode* phi2 = out->as_Phi();
      Node *incr2 = phi2->in( LoopNode::LoopBackControl );
      // Look for induction variables of the form:  X += constant
      if( phi2->region() != _head ||
          incr2->req() != 3 ||
          incr2->in(1) != phi2 ||
          incr2 == incr ||
          incr2->Opcode() != Op_AddI ||
          !incr2->in(2)->is_Con() )
        continue;

      // Check for parallel induction variable (parallel to trip counter)
      // via an affine function.  In particular, count-down loops with
      // count-up array indices are common. We only RCE references off
      // the trip-counter, so we need to convert all these to trip-counter
      // expressions.
      Node *init2 = phi2->in( LoopNode::EntryControl );
      int stride_con2 = incr2->in(2)->get_int();

      // The general case here gets a little tricky.  We want to find the
      // GCD of all possible parallel IV's and make a new IV using this
      // GCD for the loop.  Then all possible IVs are simple multiples of
      // the GCD.  In practice, this will cover very few extra loops.
      // Instead we require 'stride_con2' to be a multiple of 'stride_con',
      // where +/-1 is the common case, but other integer multiples are
      // also easy to handle.
      int ratio_con = stride_con2/stride_con;

      if( ratio_con * stride_con == stride_con2 ) { // Check for exact
        // Convert to using the trip counter.  The parallel induction
        // variable differs from the trip counter by a loop-invariant
        // amount, the difference between their respective initial values.
        // It is scaled by the 'ratio_con'.
        Compile* C = phase->C;
        Node* ratio = phase->_igvn.intcon(ratio_con);
        phase->set_ctrl(ratio, C->root());
        Node* ratio_init = new (C, 3) MulINode(init, ratio);
        phase->_igvn.register_new_node_with_optimizer(ratio_init, init);
        phase->set_early_ctrl(ratio_init);
        Node* diff = new (C, 3) SubINode(init2, ratio_init);
        phase->_igvn.register_new_node_with_optimizer(diff, init2);
        phase->set_early_ctrl(diff);
        Node* ratio_idx = new (C, 3) MulINode(phi, ratio);
        phase->_igvn.register_new_node_with_optimizer(ratio_idx, phi);
        phase->set_ctrl(ratio_idx, cl);
        Node* add  = new (C, 3) AddINode(ratio_idx, diff);
        phase->_igvn.register_new_node_with_optimizer(add);
        phase->set_ctrl(add, cl);
        phase->_igvn.hash_delete( phi2 );
        phase->_igvn.subsume_node( phi2, add );
        // Sometimes an induction variable is unused
        if (add->outcnt() == 0) {
          phase->_igvn.remove_dead_node(add);
        }
        --i; // deleted this phi; rescan starting with next position
        continue;
      }
    }
  } else if (_parent != NULL && !_irreducible) {
    // Not a counted loop.
    // Look for a safepoint on the idom-path to remove, preserving the first one
    bool found = false;
    Node* n = tail();
    for (; n != _head && !found; n = phase->idom(n)) {
      if (n->Opcode() == Op_SafePoint && phase->get_loop(n) == this)
        found = true; // Found one
    }
    // Skip past it and delete the others
    for (; n != _head; n = phase->idom(n)) {
      if (n->Opcode() == Op_SafePoint && phase->get_loop(n) == this &&
          phase->is_deleteable_safept(n))
        phase->lazy_replace(n,n->in(TypeFunc::Control));
    }
  }

  // Recursively
  if( _child ) _child->counted_loop( phase );
  if( _next  ) _next ->counted_loop( phase );
}

#ifndef PRODUCT
//------------------------------dump_head--------------------------------------
// Dump 1 liner for loop header info
void IdealLoopTree::dump_head( ) const {
  for( uint i=0; i<_nest; i++ )
    tty->print("  ");
  tty->print("Loop: N%d/N%d ",_head->_idx,_tail->_idx);
  if( _irreducible ) tty->print(" IRREDUCIBLE");
  if( _head->is_CountedLoop() ) {
    CountedLoopNode *cl = _head->as_CountedLoop();
    tty->print(" counted");
    if( cl->is_pre_loop () ) tty->print(" pre" );
    if( cl->is_main_loop() ) tty->print(" main");
    if( cl->is_post_loop() ) tty->print(" post");
  }
  tty->cr();
}

//------------------------------dump-------------------------------------------
// Dump loops by loop tree
void IdealLoopTree::dump( ) const {
  dump_head();
  if( _child ) _child->dump();
  if( _next  ) _next ->dump();
}

#endif

static void log_loop_tree(IdealLoopTree* root, IdealLoopTree* loop, CompileLog* log) {
  if (loop == root) {
    if (loop->_child != NULL) {
      log->begin_head("loop_tree");
      log->end_head();
      if( loop->_child ) log_loop_tree(root, loop->_child, log);
      log->tail("loop_tree");
      assert(loop->_next == NULL, "what?");
    }
  } else {
    Node* head = loop->_head;
    log->begin_head("loop");
    log->print(" idx='%d' ", head->_idx);
    if (loop->_irreducible) log->print("irreducible='1' ");
    if (head->is_Loop()) {
      if (head->as_Loop()->is_inner_loop()) log->print("inner_loop='1' ");
      if (head->as_Loop()->is_partial_peel_loop()) log->print("partial_peel_loop='1' ");
    }
    if (head->is_CountedLoop()) {
      CountedLoopNode* cl = head->as_CountedLoop();
      if (cl->is_pre_loop())  log->print("pre_loop='%d' ",  cl->main_idx());
      if (cl->is_main_loop()) log->print("main_loop='%d' ", cl->_idx);
      if (cl->is_post_loop()) log->print("post_loop='%d' ",  cl->main_idx());
    }
    log->end_head();
    if( loop->_child ) log_loop_tree(root, loop->_child, log);
    log->tail("loop");
    if( loop->_next  ) log_loop_tree(root, loop->_next, log);
  }
}

//=============================================================================
//----------------------------build_and_optimize-------------------------------
// Create a PhaseLoop.  Build the ideal Loop tree.  Map each Ideal Node to
// its corresponding LoopNode.  If 'optimize' is true, do some loop cleanups.
void PhaseIdealLoop::build_and_optimize(bool do_split_ifs) {
  int old_progress = C->major_progress();

  // Reset major-progress flag for the driver's heuristics
  C->clear_major_progress();

#ifndef PRODUCT
  // Capture for later assert
  uint unique = C->unique();
  _loop_invokes++;
  _loop_work += unique;
#endif

  // True if the method has at least 1 irreducible loop
  _has_irreducible_loops = false;

  _created_loop_node = false;

  Arena *a = Thread::current()->resource_area();
  VectorSet visited(a);
  // Pre-grow the mapping from Nodes to IdealLoopTrees.
  _nodes.map(C->unique(), NULL);
  memset(_nodes.adr(), 0, wordSize * C->unique());

  // Pre-build the top-level outermost loop tree entry
  _ltree_root = new IdealLoopTree( this, C->root(), C->root() );
  // Do not need a safepoint at the top level
  _ltree_root->_has_sfpt = 1;

  // Empty pre-order array
  allocate_preorders();

  // Build a loop tree on the fly.  Build a mapping from CFG nodes to
  // IdealLoopTree entries.  Data nodes are NOT walked.
  build_loop_tree();
  // Check for bailout, and return
  if (C->failing()) {
    return;
  }

  // No loops after all
  if( !_ltree_root->_child && !_verify_only ) C->set_has_loops(false);

  // There should always be an outer loop containing the Root and Return nodes.
  // If not, we have a degenerate empty program.  Bail out in this case.
  if (!has_node(C->root())) {
    if (!_verify_only) {
      C->clear_major_progress();
      C->record_method_not_compilable("empty program detected during loop optimization");
    }
    return;
  }

  // Nothing to do, so get out
  if( !C->has_loops() && !do_split_ifs && !_verify_me && !_verify_only ) {
    _igvn.optimize();           // Cleanup NeverBranches
    return;
  }

  // Set loop nesting depth
  _ltree_root->set_nest( 0 );

  // Split shared headers and insert loop landing pads.
  // Do not bother doing this on the Root loop of course.
  if( !_verify_me && !_verify_only && _ltree_root->_child ) {
    if( _ltree_root->_child->beautify_loops( this ) ) {
      // Re-build loop tree!
      _ltree_root->_child = NULL;
      _nodes.clear();
      reallocate_preorders();
      build_loop_tree();
      // Check for bailout, and return
      if (C->failing()) {
        return;
      }
      // Reset loop nesting depth
      _ltree_root->set_nest( 0 );

      C->print_method("After beautify loops", 3);
    }
  }

  // Build Dominators for elision of NULL checks & loop finding.
  // Since nodes do not have a slot for immediate dominator, make
  // a persistent side array for that info indexed on node->_idx.
  _idom_size = C->unique();
  _idom      = NEW_RESOURCE_ARRAY( Node*, _idom_size );
  _dom_depth = NEW_RESOURCE_ARRAY( uint,  _idom_size );
  _dom_stk   = NULL; // Allocated on demand in recompute_dom_depth
  memset( _dom_depth, 0, _idom_size * sizeof(uint) );

  Dominators();

  if (!_verify_only) {
    // As a side effect, Dominators removed any unreachable CFG paths
    // into RegionNodes.  It doesn't do this test against Root, so
    // we do it here.
    for( uint i = 1; i < C->root()->req(); i++ ) {
      if( !_nodes[C->root()->in(i)->_idx] ) {    // Dead path into Root?
        _igvn.hash_delete(C->root());
        C->root()->del_req(i);
        _igvn._worklist.push(C->root());
        i--;                      // Rerun same iteration on compressed edges
      }
    }

    // Given dominators, try to find inner loops with calls that must
    // always be executed (call dominates loop tail).  These loops do
    // not need a separate safepoint.
    Node_List cisstack(a);
    _ltree_root->check_safepts(visited, cisstack);
  }

  // Walk the DATA nodes and place into loops.  Find earliest control
  // node.  For CFG nodes, the _nodes array starts out and remains
  // holding the associated IdealLoopTree pointer.  For DATA nodes, the
  // _nodes array holds the earliest legal controlling CFG node.

  // Allocate stack with enough space to avoid frequent realloc
  int stack_size = (C->unique() >> 1) + 16; // (unique>>1)+16 from Java2D stats
  Node_Stack nstack( a, stack_size );

  visited.Clear();
  Node_List worklist(a);
  // Don't need C->root() on worklist since
  // it will be processed among C->top() inputs
  worklist.push( C->top() );
  visited.set( C->top()->_idx ); // Set C->top() as visited now
  build_loop_early( visited, worklist, nstack );

  // Given early legal placement, try finding counted loops.  This placement
  // is good enough to discover most loop invariants.
  if( !_verify_me && !_verify_only )
    _ltree_root->counted_loop( this );

  // Find latest loop placement.  Find ideal loop placement.
  visited.Clear();
  init_dom_lca_tags();
  // Need C->root() on worklist when processing outs
  worklist.push( C->root() );
  NOT_PRODUCT( C->verify_graph_edges(); )
  worklist.push( C->top() );
  build_loop_late( visited, worklist, nstack );

  if (_verify_only) {
    // restore major progress flag
    for (int i = 0; i < old_progress; i++)
      C->set_major_progress();
    assert(C->unique() == unique, "verification mode made Nodes? ? ?");
    assert(_igvn._worklist.size() == 0, "shouldn't push anything");
    return;
  }

  // clear out the dead code
  while(_deadlist.size()) {
    _igvn.remove_globally_dead_node(_deadlist.pop());
  }

#ifndef PRODUCT
  C->verify_graph_edges();
  if( _verify_me ) {             // Nested verify pass?
    // Check to see if the verify mode is broken
    assert(C->unique() == unique, "non-optimize mode made Nodes? ? ?");
    return;
  }
  if( VerifyLoopOptimizations ) verify();
#endif

  if (ReassociateInvariants) {
    // Reassociate invariants and prep for split_thru_phi
    for (LoopTreeIterator iter(_ltree_root); !iter.done(); iter.next()) {
      IdealLoopTree* lpt = iter.current();
      if (!lpt->is_counted() || !lpt->is_inner()) continue;

      lpt->reassociate_invariants(this);

      // Because RCE opportunities can be masked by split_thru_phi,
      // look for RCE candidates and inhibit split_thru_phi
      // on just their loop-phi's for this pass of loop opts
      if( SplitIfBlocks && do_split_ifs ) {
        if (lpt->policy_range_check(this)) {
          lpt->_rce_candidate = 1; // = true
        }
      }
    }
  }

  // Check for aggressive application of split-if and other transforms
  // that require basic-block info (like cloning through Phi's)
  if( SplitIfBlocks && do_split_ifs ) {
    visited.Clear();
    split_if_with_blocks( visited, nstack );
    NOT_PRODUCT( if( VerifyLoopOptimizations ) verify(); );
  }

  // Perform iteration-splitting on inner loops.  Split iterations to avoid
  // range checks or one-shot null checks.

  // If split-if's didn't hack the graph too bad (no CFG changes)
  // then do loop opts.
  if( C->has_loops() && !C->major_progress() ) {
    memset( worklist.adr(), 0, worklist.Size()*sizeof(Node*) );
    _ltree_root->_child->iteration_split( this, worklist );
    // No verify after peeling!  GCM has hoisted code out of the loop.
    // After peeling, the hoisted code could sink inside the peeled area.
    // The peeling code does not try to recompute the best location for
    // all the code before the peeled area, so the verify pass will always
    // complain about it.
  }
  // Do verify graph edges in any case
  NOT_PRODUCT( C->verify_graph_edges(); );

  if( !do_split_ifs ) {
    // We saw major progress in Split-If to get here.  We forced a
    // pass with unrolling and not split-if, however more split-if's
    // might make progress.  If the unrolling didn't make progress
    // then the major-progress flag got cleared and we won't try
    // another round of Split-If.  In particular the ever-common
    // instance-of/check-cast pattern requires at least 2 rounds of
    // Split-If to clear out.
    C->set_major_progress();
  }

  // Repeat loop optimizations if new loops were seen
  if (created_loop_node()) {
    C->set_major_progress();
  }

  // Convert scalar to superword operations

  if (UseSuperWord && C->has_loops() && !C->major_progress()) {
    // SuperWord transform
    SuperWord sw(this);
    for (LoopTreeIterator iter(_ltree_root); !iter.done(); iter.next()) {
      IdealLoopTree* lpt = iter.current();
      if (lpt->is_counted()) {
        sw.transform_loop(lpt);
      }
    }
  }

  // Cleanup any modified bits
  _igvn.optimize();

  // disable assert until issue with split_flow_path is resolved (6742111)
  // assert(!_has_irreducible_loops || C->parsed_irreducible_loop() || C->is_osr_compilation(),
  //        "shouldn't introduce irreducible loops");

  if (C->log() != NULL) {
    log_loop_tree(_ltree_root, _ltree_root, C->log());
  }
}

#ifndef PRODUCT
//------------------------------print_statistics-------------------------------
int PhaseIdealLoop::_loop_invokes=0;// Count of PhaseIdealLoop invokes
int PhaseIdealLoop::_loop_work=0; // Sum of PhaseIdealLoop x unique
void PhaseIdealLoop::print_statistics() {
  tty->print_cr("PhaseIdealLoop=%d, sum _unique=%d", _loop_invokes, _loop_work);
}

//------------------------------verify-----------------------------------------
// Build a verify-only PhaseIdealLoop, and see that it agrees with me.
static int fail;                // debug only, so its multi-thread dont care
void PhaseIdealLoop::verify() const {
  int old_progress = C->major_progress();
  ResourceMark rm;
  PhaseIdealLoop loop_verify( _igvn, this );
  VectorSet visited(Thread::current()->resource_area());

  fail = 0;
  verify_compare( C->root(), &loop_verify, visited );
  assert( fail == 0, "verify loops failed" );
  // Verify loop structure is the same
  _ltree_root->verify_tree(loop_verify._ltree_root, NULL);
  // Reset major-progress.  It was cleared by creating a verify version of
  // PhaseIdealLoop.
  for( int i=0; i<old_progress; i++ )
    C->set_major_progress();
}

//------------------------------verify_compare---------------------------------
// Make sure me and the given PhaseIdealLoop agree on key data structures
void PhaseIdealLoop::verify_compare( Node *n, const PhaseIdealLoop *loop_verify, VectorSet &visited ) const {
  if( !n ) return;
  if( visited.test_set( n->_idx ) ) return;
  if( !_nodes[n->_idx] ) {      // Unreachable
    assert( !loop_verify->_nodes[n->_idx], "both should be unreachable" );
    return;
  }

  uint i;
  for( i = 0; i < n->req(); i++ )
    verify_compare( n->in(i), loop_verify, visited );

  // Check the '_nodes' block/loop structure
  i = n->_idx;
  if( has_ctrl(n) ) {           // We have control; verify has loop or ctrl
    if( _nodes[i] != loop_verify->_nodes[i] &&
        get_ctrl_no_update(n) != loop_verify->get_ctrl_no_update(n) ) {
      tty->print("Mismatched control setting for: ");
      n->dump();
      if( fail++ > 10 ) return;
      Node *c = get_ctrl_no_update(n);
      tty->print("We have it as: ");
      if( c->in(0) ) c->dump();
        else tty->print_cr("N%d",c->_idx);
      tty->print("Verify thinks: ");
      if( loop_verify->has_ctrl(n) )
        loop_verify->get_ctrl_no_update(n)->dump();
      else
        loop_verify->get_loop_idx(n)->dump();
      tty->cr();
    }
  } else {                    // We have a loop
    IdealLoopTree *us = get_loop_idx(n);
    if( loop_verify->has_ctrl(n) ) {
      tty->print("Mismatched loop setting for: ");
      n->dump();
      if( fail++ > 10 ) return;
      tty->print("We have it as: ");
      us->dump();
      tty->print("Verify thinks: ");
      loop_verify->get_ctrl_no_update(n)->dump();
      tty->cr();
    } else if (!C->major_progress()) {
      // Loop selection can be messed up if we did a major progress
      // operation, like split-if.  Do not verify in that case.
      IdealLoopTree *them = loop_verify->get_loop_idx(n);
      if( us->_head != them->_head ||  us->_tail != them->_tail ) {
        tty->print("Unequals loops for: ");
        n->dump();
        if( fail++ > 10 ) return;
        tty->print("We have it as: ");
        us->dump();
        tty->print("Verify thinks: ");
        them->dump();
        tty->cr();
      }
    }
  }

  // Check for immediate dominators being equal
  if( i >= _idom_size ) {
    if( !n->is_CFG() ) return;
    tty->print("CFG Node with no idom: ");
    n->dump();
    return;
  }
  if( !n->is_CFG() ) return;
  if( n == C->root() ) return; // No IDOM here

  assert(n->_idx == i, "sanity");
  Node *id = idom_no_update(n);
  if( id != loop_verify->idom_no_update(n) ) {
    tty->print("Unequals idoms for: ");
    n->dump();
    if( fail++ > 10 ) return;
    tty->print("We have it as: ");
    id->dump();
    tty->print("Verify thinks: ");
    loop_verify->idom_no_update(n)->dump();
    tty->cr();
  }

}

//------------------------------verify_tree------------------------------------
// Verify that tree structures match.  Because the CFG can change, siblings
// within the loop tree can be reordered.  We attempt to deal with that by
// reordering the verify's loop tree if possible.
void IdealLoopTree::verify_tree(IdealLoopTree *loop, const IdealLoopTree *parent) const {
  assert( _parent == parent, "Badly formed loop tree" );

  // Siblings not in same order?  Attempt to re-order.
  if( _head != loop->_head ) {
    // Find _next pointer to update
    IdealLoopTree **pp = &loop->_parent->_child;
    while( *pp != loop )
      pp = &((*pp)->_next);
    // Find proper sibling to be next
    IdealLoopTree **nn = &loop->_next;
    while( (*nn) && (*nn)->_head != _head )
      nn = &((*nn)->_next);

    // Check for no match.
    if( !(*nn) ) {
      // Annoyingly, irreducible loops can pick different headers
      // after a major_progress operation, so the rest of the loop
      // tree cannot be matched.
      if (_irreducible && Compile::current()->major_progress())  return;
      assert( 0, "failed to match loop tree" );
    }

    // Move (*nn) to (*pp)
    IdealLoopTree *hit = *nn;
    *nn = hit->_next;
    hit->_next = loop;
    *pp = loop;
    loop = hit;
    // Now try again to verify
  }

  assert( _head  == loop->_head , "mismatched loop head" );
  Node *tail = _tail;           // Inline a non-updating version of
  while( !tail->in(0) )         // the 'tail()' call.
    tail = tail->in(1);
  assert( tail == loop->_tail, "mismatched loop tail" );

  // Counted loops that are guarded should be able to find their guards
  if( _head->is_CountedLoop() && _head->as_CountedLoop()->is_main_loop() ) {
    CountedLoopNode *cl = _head->as_CountedLoop();
    Node *init = cl->init_trip();
    Node *ctrl = cl->in(LoopNode::EntryControl);
    assert( ctrl->Opcode() == Op_IfTrue || ctrl->Opcode() == Op_IfFalse, "" );
    Node *iff  = ctrl->in(0);
    assert( iff->Opcode() == Op_If, "" );
    Node *bol  = iff->in(1);
    assert( bol->Opcode() == Op_Bool, "" );
    Node *cmp  = bol->in(1);
    assert( cmp->Opcode() == Op_CmpI, "" );
    Node *add  = cmp->in(1);
    Node *opaq;
    if( add->Opcode() == Op_Opaque1 ) {
      opaq = add;
    } else {
      assert( add->Opcode() == Op_AddI || add->Opcode() == Op_ConI , "" );
      assert( add == init, "" );
      opaq = cmp->in(2);
    }
    assert( opaq->Opcode() == Op_Opaque1, "" );

  }

  if (_child != NULL)  _child->verify_tree(loop->_child, this);
  if (_next  != NULL)  _next ->verify_tree(loop->_next,  parent);
  // Innermost loops need to verify loop bodies,
  // but only if no 'major_progress'
  int fail = 0;
  if (!Compile::current()->major_progress() && _child == NULL) {
    for( uint i = 0; i < _body.size(); i++ ) {
      Node *n = _body.at(i);
      if (n->outcnt() == 0)  continue; // Ignore dead
      uint j;
      for( j = 0; j < loop->_body.size(); j++ )
        if( loop->_body.at(j) == n )
          break;
      if( j == loop->_body.size() ) { // Not found in loop body
        // Last ditch effort to avoid assertion: Its possible that we
        // have some users (so outcnt not zero) but are still dead.
        // Try to find from root.
        if (Compile::current()->root()->find(n->_idx)) {
          fail++;
          tty->print("We have that verify does not: ");
          n->dump();
        }
      }
    }
    for( uint i2 = 0; i2 < loop->_body.size(); i2++ ) {
      Node *n = loop->_body.at(i2);
      if (n->outcnt() == 0)  continue; // Ignore dead
      uint j;
      for( j = 0; j < _body.size(); j++ )
        if( _body.at(j) == n )
          break;
      if( j == _body.size() ) { // Not found in loop body
        // Last ditch effort to avoid assertion: Its possible that we
        // have some users (so outcnt not zero) but are still dead.
        // Try to find from root.
        if (Compile::current()->root()->find(n->_idx)) {
          fail++;
          tty->print("Verify has that we do not: ");
          n->dump();
        }
      }
    }
    assert( !fail, "loop body mismatch" );
  }
}

#endif

//------------------------------set_idom---------------------------------------
void PhaseIdealLoop::set_idom(Node* d, Node* n, uint dom_depth) {
  uint idx = d->_idx;
  if (idx >= _idom_size) {
    uint newsize = _idom_size<<1;
    while( idx >= newsize ) {
      newsize <<= 1;
    }
    _idom      = REALLOC_RESOURCE_ARRAY( Node*,     _idom,_idom_size,newsize);
    _dom_depth = REALLOC_RESOURCE_ARRAY( uint, _dom_depth,_idom_size,newsize);
    memset( _dom_depth + _idom_size, 0, (newsize - _idom_size) * sizeof(uint) );
    _idom_size = newsize;
  }
  _idom[idx] = n;
  _dom_depth[idx] = dom_depth;
}

//------------------------------recompute_dom_depth---------------------------------------
// The dominator tree is constructed with only parent pointers.
// This recomputes the depth in the tree by first tagging all
// nodes as "no depth yet" marker.  The next pass then runs up
// the dom tree from each node marked "no depth yet", and computes
// the depth on the way back down.
void PhaseIdealLoop::recompute_dom_depth() {
  uint no_depth_marker = C->unique();
  uint i;
  // Initialize depth to "no depth yet"
  for (i = 0; i < _idom_size; i++) {
    if (_dom_depth[i] > 0 && _idom[i] != NULL) {
     _dom_depth[i] = no_depth_marker;
    }
  }
  if (_dom_stk == NULL) {
    uint init_size = C->unique() / 100; // Guess that 1/100 is a reasonable initial size.
    if (init_size < 10) init_size = 10;
    _dom_stk = new (C->node_arena()) GrowableArray<uint>(C->node_arena(), init_size, 0, 0);
  }
  // Compute new depth for each node.
  for (i = 0; i < _idom_size; i++) {
    uint j = i;
    // Run up the dom tree to find a node with a depth
    while (_dom_depth[j] == no_depth_marker) {
      _dom_stk->push(j);
      j = _idom[j]->_idx;
    }
    // Compute the depth on the way back down this tree branch
    uint dd = _dom_depth[j] + 1;
    while (_dom_stk->length() > 0) {
      uint j = _dom_stk->pop();
      _dom_depth[j] = dd;
      dd++;
    }
  }
}

//------------------------------sort-------------------------------------------
// Insert 'loop' into the existing loop tree.  'innermost' is a leaf of the
// loop tree, not the root.
IdealLoopTree *PhaseIdealLoop::sort( IdealLoopTree *loop, IdealLoopTree *innermost ) {
  if( !innermost ) return loop; // New innermost loop

  int loop_preorder = get_preorder(loop->_head); // Cache pre-order number
  assert( loop_preorder, "not yet post-walked loop" );
  IdealLoopTree **pp = &innermost;      // Pointer to previous next-pointer
  IdealLoopTree *l = *pp;               // Do I go before or after 'l'?

  // Insert at start of list
  while( l ) {                  // Insertion sort based on pre-order
    if( l == loop ) return innermost; // Already on list!
    int l_preorder = get_preorder(l->_head); // Cache pre-order number
    assert( l_preorder, "not yet post-walked l" );
    // Check header pre-order number to figure proper nesting
    if( loop_preorder > l_preorder )
      break;                    // End of insertion
    // If headers tie (e.g., shared headers) check tail pre-order numbers.
    // Since I split shared headers, you'd think this could not happen.
    // BUT: I must first do the preorder numbering before I can discover I
    // have shared headers, so the split headers all get the same preorder
    // number as the RegionNode they split from.
    if( loop_preorder == l_preorder &&
        get_preorder(loop->_tail) < get_preorder(l->_tail) )
      break;                    // Also check for shared headers (same pre#)
    pp = &l->_parent;           // Chain up list
    l = *pp;
  }
  // Link into list
  // Point predecessor to me
  *pp = loop;
  // Point me to successor
  IdealLoopTree *p = loop->_parent;
  loop->_parent = l;            // Point me to successor
  if( p ) sort( p, innermost ); // Insert my parents into list as well
  return innermost;
}

//------------------------------build_loop_tree--------------------------------
// I use a modified Vick/Tarjan algorithm.  I need pre- and a post- visit
// bits.  The _nodes[] array is mapped by Node index and holds a NULL for
// not-yet-pre-walked, pre-order # for pre-but-not-post-walked and holds the
// tightest enclosing IdealLoopTree for post-walked.
//
// During my forward walk I do a short 1-layer lookahead to see if I can find
// a loop backedge with that doesn't have any work on the backedge.  This
// helps me construct nested loops with shared headers better.
//
// Once I've done the forward recursion, I do the post-work.  For each child
// I check to see if there is a backedge.  Backedges define a loop!  I
// insert an IdealLoopTree at the target of the backedge.
//
// During the post-work I also check to see if I have several children
// belonging to different loops.  If so, then this Node is a decision point
// where control flow can choose to change loop nests.  It is at this
// decision point where I can figure out how loops are nested.  At this
// time I can properly order the different loop nests from my children.
// Note that there may not be any backedges at the decision point!
//
// Since the decision point can be far removed from the backedges, I can't
// order my loops at the time I discover them.  Thus at the decision point
// I need to inspect loop header pre-order numbers to properly nest my
// loops.  This means I need to sort my childrens' loops by pre-order.
// The sort is of size number-of-control-children, which generally limits
// it to size 2 (i.e., I just choose between my 2 target loops).
void PhaseIdealLoop::build_loop_tree() {
  // Allocate stack of size C->unique()/2 to avoid frequent realloc
  GrowableArray <Node *> bltstack(C->unique() >> 1);
  Node *n = C->root();
  bltstack.push(n);
  int pre_order = 1;
  int stack_size;

  while ( ( stack_size = bltstack.length() ) != 0 ) {
    n = bltstack.top(); // Leave node on stack
    if ( !is_visited(n) ) {
      // ---- Pre-pass Work ----
      // Pre-walked but not post-walked nodes need a pre_order number.

      set_preorder_visited( n, pre_order ); // set as visited

      // ---- Scan over children ----
      // Scan first over control projections that lead to loop headers.
      // This helps us find inner-to-outer loops with shared headers better.

      // Scan children's children for loop headers.
      for ( int i = n->outcnt() - 1; i >= 0; --i ) {
        Node* m = n->raw_out(i);       // Child
        if( m->is_CFG() && !is_visited(m) ) { // Only for CFG children
          // Scan over children's children to find loop
          for (DUIterator_Fast jmax, j = m->fast_outs(jmax); j < jmax; j++) {
            Node* l = m->fast_out(j);
            if( is_visited(l) &&       // Been visited?
                !is_postvisited(l) &&  // But not post-visited
                get_preorder(l) < pre_order ) { // And smaller pre-order
              // Found!  Scan the DFS down this path before doing other paths
              bltstack.push(m);
              break;
            }
          }
        }
      }
      pre_order++;
    }
    else if ( !is_postvisited(n) ) {
      // Note: build_loop_tree_impl() adds out edges on rare occasions,
      // such as com.sun.rsasign.am::a.
      // For non-recursive version, first, process current children.
      // On next iteration, check if additional children were added.
      for ( int k = n->outcnt() - 1; k >= 0; --k ) {
        Node* u = n->raw_out(k);
        if ( u->is_CFG() && !is_visited(u) ) {
          bltstack.push(u);
        }
      }
      if ( bltstack.length() == stack_size ) {
        // There were no additional children, post visit node now
        (void)bltstack.pop(); // Remove node from stack
        pre_order = build_loop_tree_impl( n, pre_order );
        // Check for bailout
        if (C->failing()) {
          return;
        }
        // Check to grow _preorders[] array for the case when
        // build_loop_tree_impl() adds new nodes.
        check_grow_preorders();
      }
    }
    else {
      (void)bltstack.pop(); // Remove post-visited node from stack
    }
  }
}

//------------------------------build_loop_tree_impl---------------------------
int PhaseIdealLoop::build_loop_tree_impl( Node *n, int pre_order ) {
  // ---- Post-pass Work ----
  // Pre-walked but not post-walked nodes need a pre_order number.

  // Tightest enclosing loop for this Node
  IdealLoopTree *innermost = NULL;

  // For all children, see if any edge is a backedge.  If so, make a loop
  // for it.  Then find the tightest enclosing loop for the self Node.
  for (DUIterator_Fast imax, i = n->fast_outs(imax); i < imax; i++) {
    Node* m = n->fast_out(i);   // Child
    if( n == m ) continue;      // Ignore control self-cycles
    if( !m->is_CFG() ) continue;// Ignore non-CFG edges

    IdealLoopTree *l;           // Child's loop
    if( !is_postvisited(m) ) {  // Child visited but not post-visited?
      // Found a backedge
      assert( get_preorder(m) < pre_order, "should be backedge" );
      // Check for the RootNode, which is already a LoopNode and is allowed
      // to have multiple "backedges".
      if( m == C->root()) {     // Found the root?
        l = _ltree_root;        // Root is the outermost LoopNode
      } else {                  // Else found a nested loop
        // Insert a LoopNode to mark this loop.
        l = new IdealLoopTree(this, m, n);
      } // End of Else found a nested loop
      if( !has_loop(m) )        // If 'm' does not already have a loop set
        set_loop(m, l);         // Set loop header to loop now

    } else {                    // Else not a nested loop
      if( !_nodes[m->_idx] ) continue; // Dead code has no loop
      l = get_loop(m);          // Get previously determined loop
      // If successor is header of a loop (nest), move up-loop till it
      // is a member of some outer enclosing loop.  Since there are no
      // shared headers (I've split them already) I only need to go up
      // at most 1 level.
      while( l && l->_head == m ) // Successor heads loop?
        l = l->_parent;         // Move up 1 for me
      // If this loop is not properly parented, then this loop
      // has no exit path out, i.e. its an infinite loop.
      if( !l ) {
        // Make loop "reachable" from root so the CFG is reachable.  Basically
        // insert a bogus loop exit that is never taken.  'm', the loop head,
        // points to 'n', one (of possibly many) fall-in paths.  There may be
        // many backedges as well.

        // Here I set the loop to be the root loop.  I could have, after
        // inserting a bogus loop exit, restarted the recursion and found my
        // new loop exit.  This would make the infinite loop a first-class
        // loop and it would then get properly optimized.  What's the use of
        // optimizing an infinite loop?
        l = _ltree_root;        // Oops, found infinite loop

        if (!_verify_only) {
          // Insert the NeverBranch between 'm' and it's control user.
          NeverBranchNode *iff = new (C, 1) NeverBranchNode( m );
          _igvn.register_new_node_with_optimizer(iff);
          set_loop(iff, l);
          Node *if_t = new (C, 1) CProjNode( iff, 0 );
          _igvn.register_new_node_with_optimizer(if_t);
          set_loop(if_t, l);

          Node* cfg = NULL;       // Find the One True Control User of m
          for (DUIterator_Fast jmax, j = m->fast_outs(jmax); j < jmax; j++) {
            Node* x = m->fast_out(j);
            if (x->is_CFG() && x != m && x != iff)
              { cfg = x; break; }
          }
          assert(cfg != NULL, "must find the control user of m");
          uint k = 0;             // Probably cfg->in(0)
          while( cfg->in(k) != m ) k++; // But check incase cfg is a Region
          cfg->set_req( k, if_t ); // Now point to NeverBranch

          // Now create the never-taken loop exit
          Node *if_f = new (C, 1) CProjNode( iff, 1 );
          _igvn.register_new_node_with_optimizer(if_f);
          set_loop(if_f, l);
          // Find frame ptr for Halt.  Relies on the optimizer
          // V-N'ing.  Easier and quicker than searching through
          // the program structure.
          Node *frame = new (C, 1) ParmNode( C->start(), TypeFunc::FramePtr );
          _igvn.register_new_node_with_optimizer(frame);
          // Halt & Catch Fire
          Node *halt = new (C, TypeFunc::Parms) HaltNode( if_f, frame );
          _igvn.register_new_node_with_optimizer(halt);
          set_loop(halt, l);
          C->root()->add_req(halt);
        }
        set_loop(C->root(), _ltree_root);
      }
    }
    // Weeny check for irreducible.  This child was already visited (this
    // IS the post-work phase).  Is this child's loop header post-visited
    // as well?  If so, then I found another entry into the loop.
    if (!_verify_only) {
      while( is_postvisited(l->_head) ) {
        // found irreducible
        l->_irreducible = 1; // = true
        l = l->_parent;
        _has_irreducible_loops = true;
        // Check for bad CFG here to prevent crash, and bailout of compile
        if (l == NULL) {
          C->record_method_not_compilable("unhandled CFG detected during loop optimization");
          return pre_order;
        }
      }
    }

    // This Node might be a decision point for loops.  It is only if
    // it's children belong to several different loops.  The sort call
    // does a trivial amount of work if there is only 1 child or all
    // children belong to the same loop.  If however, the children
    // belong to different loops, the sort call will properly set the
    // _parent pointers to show how the loops nest.
    //
    // In any case, it returns the tightest enclosing loop.
    innermost = sort( l, innermost );
  }

  // Def-use info will have some dead stuff; dead stuff will have no
  // loop decided on.

  // Am I a loop header?  If so fix up my parent's child and next ptrs.
  if( innermost && innermost->_head == n ) {
    assert( get_loop(n) == innermost, "" );
    IdealLoopTree *p = innermost->_parent;
    IdealLoopTree *l = innermost;
    while( p && l->_head == n ) {
      l->_next = p->_child;     // Put self on parents 'next child'
      p->_child = l;            // Make self as first child of parent
      l = p;                    // Now walk up the parent chain
      p = l->_parent;
    }
  } else {
    // Note that it is possible for a LoopNode to reach here, if the
    // backedge has been made unreachable (hence the LoopNode no longer
    // denotes a Loop, and will eventually be removed).

    // Record tightest enclosing loop for self.  Mark as post-visited.
    set_loop(n, innermost);
    // Also record has_call flag early on
    if( innermost ) {
      if( n->is_Call() && !n->is_CallLeaf() && !n->is_macro() ) {
        // Do not count uncommon calls
        if( !n->is_CallStaticJava() || !n->as_CallStaticJava()->_name ) {
          Node *iff = n->in(0)->in(0);
          if( !iff->is_If() ||
              (n->in(0)->Opcode() == Op_IfFalse &&
               (1.0 - iff->as_If()->_prob) >= 0.01) ||
              (iff->as_If()->_prob >= 0.01) )
            innermost->_has_call = 1;
        }
      } else if( n->is_Allocate() && n->as_Allocate()->_is_scalar_replaceable ) {
        // Disable loop optimizations if the loop has a scalar replaceable
        // allocation. This disabling may cause a potential performance lost
        // if the allocation is not eliminated for some reason.
        innermost->_allow_optimizations = false;
        innermost->_has_call = 1; // = true
      }
    }
  }

  // Flag as post-visited now
  set_postvisited(n);
  return pre_order;
}


//------------------------------build_loop_early-------------------------------
// Put Data nodes into some loop nest, by setting the _nodes[]->loop mapping.
// First pass computes the earliest controlling node possible.  This is the
// controlling input with the deepest dominating depth.
void PhaseIdealLoop::build_loop_early( VectorSet &visited, Node_List &worklist, Node_Stack &nstack ) {
  while (worklist.size() != 0) {
    // Use local variables nstack_top_n & nstack_top_i to cache values
    // on nstack's top.
    Node *nstack_top_n = worklist.pop();
    uint  nstack_top_i = 0;
//while_nstack_nonempty:
    while (true) {
      // Get parent node and next input's index from stack's top.
      Node  *n = nstack_top_n;
      uint   i = nstack_top_i;
      uint cnt = n->req(); // Count of inputs
      if (i == 0) {        // Pre-process the node.
        if( has_node(n) &&            // Have either loop or control already?
            !has_ctrl(n) ) {          // Have loop picked out already?
          // During "merge_many_backedges" we fold up several nested loops
          // into a single loop.  This makes the members of the original
          // loop bodies pointing to dead loops; they need to move up
          // to the new UNION'd larger loop.  I set the _head field of these
          // dead loops to NULL and the _parent field points to the owning
          // loop.  Shades of UNION-FIND algorithm.
          IdealLoopTree *ilt;
          while( !(ilt = get_loop(n))->_head ) {
            // Normally I would use a set_loop here.  But in this one special
            // case, it is legal (and expected) to change what loop a Node
            // belongs to.
            _nodes.map(n->_idx, (Node*)(ilt->_parent) );
          }
          // Remove safepoints ONLY if I've already seen I don't need one.
          // (the old code here would yank a 2nd safepoint after seeing a
          // first one, even though the 1st did not dominate in the loop body
          // and thus could be avoided indefinitely)
          if( !_verify_only && !_verify_me && ilt->_has_sfpt && n->Opcode() == Op_SafePoint &&
              is_deleteable_safept(n)) {
            Node *in = n->in(TypeFunc::Control);
            lazy_replace(n,in);       // Pull safepoint now
            // Carry on with the recursion "as if" we are walking
            // only the control input
            if( !visited.test_set( in->_idx ) ) {
              worklist.push(in);      // Visit this guy later, using worklist
            }
            // Get next node from nstack:
            // - skip n's inputs processing by setting i > cnt;
            // - we also will not call set_early_ctrl(n) since
            //   has_node(n) == true (see the condition above).
            i = cnt + 1;
          }
        }
      } // if (i == 0)

      // Visit all inputs
      bool done = true;       // Assume all n's inputs will be processed
      while (i < cnt) {
        Node *in = n->in(i);
        ++i;
        if (in == NULL) continue;
        if (in->pinned() && !in->is_CFG())
          set_ctrl(in, in->in(0));
        int is_visited = visited.test_set( in->_idx );
        if (!has_node(in)) {  // No controlling input yet?
          assert( !in->is_CFG(), "CFG Node with no controlling input?" );
          assert( !is_visited, "visit only once" );
          nstack.push(n, i);  // Save parent node and next input's index.
          nstack_top_n = in;  // Process current input now.
          nstack_top_i = 0;
          done = false;       // Not all n's inputs processed.
          break; // continue while_nstack_nonempty;
        } else if (!is_visited) {
          // This guy has a location picked out for him, but has not yet
          // been visited.  Happens to all CFG nodes, for instance.
          // Visit him using the worklist instead of recursion, to break
          // cycles.  Since he has a location already we do not need to
          // find his location before proceeding with the current Node.
          worklist.push(in);  // Visit this guy later, using worklist
        }
      }
      if (done) {
        // All of n's inputs have been processed, complete post-processing.

        // Compute earliest point this Node can go.
        // CFG, Phi, pinned nodes already know their controlling input.
        if (!has_node(n)) {
          // Record earliest legal location
          set_early_ctrl( n );
        }
        if (nstack.is_empty()) {
          // Finished all nodes on stack.
          // Process next node on the worklist.
          break;
        }
        // Get saved parent node and next input's index.
        nstack_top_n = nstack.node();
        nstack_top_i = nstack.index();
        nstack.pop();
      }
    } // while (true)
  }
}

//------------------------------dom_lca_internal--------------------------------
// Pair-wise LCA
Node *PhaseIdealLoop::dom_lca_internal( Node *n1, Node *n2 ) const {
  if( !n1 ) return n2;          // Handle NULL original LCA
  assert( n1->is_CFG(), "" );
  assert( n2->is_CFG(), "" );
  // find LCA of all uses
  uint d1 = dom_depth(n1);
  uint d2 = dom_depth(n2);
  while (n1 != n2) {
    if (d1 > d2) {
      n1 =      idom(n1);
      d1 = dom_depth(n1);
    } else if (d1 < d2) {
      n2 =      idom(n2);
      d2 = dom_depth(n2);
    } else {
      // Here d1 == d2.  Due to edits of the dominator-tree, sections
      // of the tree might have the same depth.  These sections have
      // to be searched more carefully.

      // Scan up all the n1's with equal depth, looking for n2.
      Node *t1 = idom(n1);
      while (dom_depth(t1) == d1) {
        if (t1 == n2)  return n2;
        t1 = idom(t1);
      }
      // Scan up all the n2's with equal depth, looking for n1.
      Node *t2 = idom(n2);
      while (dom_depth(t2) == d2) {
        if (t2 == n1)  return n1;
        t2 = idom(t2);
      }
      // Move up to a new dominator-depth value as well as up the dom-tree.
      n1 = t1;
      n2 = t2;
      d1 = dom_depth(n1);
      d2 = dom_depth(n2);
    }
  }
  return n1;
}

//------------------------------compute_idom-----------------------------------
// Locally compute IDOM using dom_lca call.  Correct only if the incoming
// IDOMs are correct.
Node *PhaseIdealLoop::compute_idom( Node *region ) const {
  assert( region->is_Region(), "" );
  Node *LCA = NULL;
  for( uint i = 1; i < region->req(); i++ ) {
    if( region->in(i) != C->top() )
      LCA = dom_lca( LCA, region->in(i) );
  }
  return LCA;
}

bool PhaseIdealLoop::verify_dominance(Node* n, Node* use, Node* LCA, Node* early) {
  bool had_error = false;
#ifdef ASSERT
  if (early != C->root()) {
    // Make sure that there's a dominance path from use to LCA
    Node* d = use;
    while (d != LCA) {
      d = idom(d);
      if (d == C->root()) {
        tty->print_cr("*** Use %d isn't dominated by def %s", use->_idx, n->_idx);
        n->dump();
        use->dump();
        had_error = true;
        break;
      }
    }
  }
#endif
  return had_error;
}


Node* PhaseIdealLoop::compute_lca_of_uses(Node* n, Node* early, bool verify) {
  // Compute LCA over list of uses
  bool had_error = false;
  Node *LCA = NULL;
  for (DUIterator_Fast imax, i = n->fast_outs(imax); i < imax && LCA != early; i++) {
    Node* c = n->fast_out(i);
    if (_nodes[c->_idx] == NULL)
      continue;                 // Skip the occasional dead node
    if( c->is_Phi() ) {         // For Phis, we must land above on the path
      for( uint j=1; j<c->req(); j++ ) {// For all inputs
        if( c->in(j) == n ) {   // Found matching input?
          Node *use = c->in(0)->in(j);
          if (_verify_only && use->is_top()) continue;
          LCA = dom_lca_for_get_late_ctrl( LCA, use, n );
          if (verify) had_error = verify_dominance(n, use, LCA, early) || had_error;
        }
      }
    } else {
      // For CFG data-users, use is in the block just prior
      Node *use = has_ctrl(c) ? get_ctrl(c) : c->in(0);
      LCA = dom_lca_for_get_late_ctrl( LCA, use, n );
      if (verify) had_error = verify_dominance(n, use, LCA, early) || had_error;
    }
  }
  assert(!had_error, "bad dominance");
  return LCA;
}

//------------------------------get_late_ctrl----------------------------------
// Compute latest legal control.
Node *PhaseIdealLoop::get_late_ctrl( Node *n, Node *early ) {
  assert(early != NULL, "early control should not be NULL");

  Node* LCA = compute_lca_of_uses(n, early);
#ifdef ASSERT
  if (LCA == C->root() && LCA != early) {
    // def doesn't dominate uses so print some useful debugging output
    compute_lca_of_uses(n, early, true);
  }
#endif

  // if this is a load, check for anti-dependent stores
  // We use a conservative algorithm to identify potential interfering
  // instructions and for rescheduling the load.  The users of the memory
  // input of this load are examined.  Any use which is not a load and is
  // dominated by early is considered a potentially interfering store.
  // This can produce false positives.
  if (n->is_Load() && LCA != early) {
    Node_List worklist;

    Node *mem = n->in(MemNode::Memory);
    for (DUIterator_Fast imax, i = mem->fast_outs(imax); i < imax; i++) {
      Node* s = mem->fast_out(i);
      worklist.push(s);
    }
    while(worklist.size() != 0 && LCA != early) {
      Node* s = worklist.pop();
      if (s->is_Load()) {
        continue;
      } else if (s->is_MergeMem()) {
        for (DUIterator_Fast imax, i = s->fast_outs(imax); i < imax; i++) {
          Node* s1 = s->fast_out(i);
          worklist.push(s1);
        }
      } else {
        Node *sctrl = has_ctrl(s) ? get_ctrl(s) : s->in(0);
        assert(sctrl != NULL || s->outcnt() == 0, "must have control");
        if (sctrl != NULL && !sctrl->is_top() && is_dominator(early, sctrl)) {
          LCA = dom_lca_for_get_late_ctrl(LCA, sctrl, n);
        }
      }
    }
  }

  assert(LCA == find_non_split_ctrl(LCA), "unexpected late control");
  return LCA;
}

// true if CFG node d dominates CFG node n
bool PhaseIdealLoop::is_dominator(Node *d, Node *n) {
  if (d == n)
    return true;
  assert(d->is_CFG() && n->is_CFG(), "must have CFG nodes");
  uint dd = dom_depth(d);
  while (dom_depth(n) >= dd) {
    if (n == d)
      return true;
    n = idom(n);
  }
  return false;
}

//------------------------------dom_lca_for_get_late_ctrl_internal-------------
// Pair-wise LCA with tags.
// Tag each index with the node 'tag' currently being processed
// before advancing up the dominator chain using idom().
// Later calls that find a match to 'tag' know that this path has already
// been considered in the current LCA (which is input 'n1' by convention).
// Since get_late_ctrl() is only called once for each node, the tag array
// does not need to be cleared between calls to get_late_ctrl().
// Algorithm trades a larger constant factor for better asymptotic behavior
//
Node *PhaseIdealLoop::dom_lca_for_get_late_ctrl_internal( Node *n1, Node *n2, Node *tag ) {
  uint d1 = dom_depth(n1);
  uint d2 = dom_depth(n2);

  do {
    if (d1 > d2) {
      // current lca is deeper than n2
      _dom_lca_tags.map(n1->_idx, tag);
      n1 =      idom(n1);
      d1 = dom_depth(n1);
    } else if (d1 < d2) {
      // n2 is deeper than current lca
      Node *memo = _dom_lca_tags[n2->_idx];
      if( memo == tag ) {
        return n1;    // Return the current LCA
      }
      _dom_lca_tags.map(n2->_idx, tag);
      n2 =      idom(n2);
      d2 = dom_depth(n2);
    } else {
      // Here d1 == d2.  Due to edits of the dominator-tree, sections
      // of the tree might have the same depth.  These sections have
      // to be searched more carefully.

      // Scan up all the n1's with equal depth, looking for n2.
      _dom_lca_tags.map(n1->_idx, tag);
      Node *t1 = idom(n1);
      while (dom_depth(t1) == d1) {
        if (t1 == n2)  return n2;
        _dom_lca_tags.map(t1->_idx, tag);
        t1 = idom(t1);
      }
      // Scan up all the n2's with equal depth, looking for n1.
      _dom_lca_tags.map(n2->_idx, tag);
      Node *t2 = idom(n2);
      while (dom_depth(t2) == d2) {
        if (t2 == n1)  return n1;
        _dom_lca_tags.map(t2->_idx, tag);
        t2 = idom(t2);
      }
      // Move up to a new dominator-depth value as well as up the dom-tree.
      n1 = t1;
      n2 = t2;
      d1 = dom_depth(n1);
      d2 = dom_depth(n2);
    }
  } while (n1 != n2);
  return n1;
}

//------------------------------init_dom_lca_tags------------------------------
// Tag could be a node's integer index, 32bits instead of 64bits in some cases
// Intended use does not involve any growth for the array, so it could
// be of fixed size.
void PhaseIdealLoop::init_dom_lca_tags() {
  uint limit = C->unique() + 1;
  _dom_lca_tags.map( limit, NULL );
#ifdef ASSERT
  for( uint i = 0; i < limit; ++i ) {
    assert(_dom_lca_tags[i] == NULL, "Must be distinct from each node pointer");
  }
#endif // ASSERT
}

//------------------------------clear_dom_lca_tags------------------------------
// Tag could be a node's integer index, 32bits instead of 64bits in some cases
// Intended use does not involve any growth for the array, so it could
// be of fixed size.
void PhaseIdealLoop::clear_dom_lca_tags() {
  uint limit = C->unique() + 1;
  _dom_lca_tags.map( limit, NULL );
  _dom_lca_tags.clear();
#ifdef ASSERT
  for( uint i = 0; i < limit; ++i ) {
    assert(_dom_lca_tags[i] == NULL, "Must be distinct from each node pointer");
  }
#endif // ASSERT
}

//------------------------------build_loop_late--------------------------------
// Put Data nodes into some loop nest, by setting the _nodes[]->loop mapping.
// Second pass finds latest legal placement, and ideal loop placement.
void PhaseIdealLoop::build_loop_late( VectorSet &visited, Node_List &worklist, Node_Stack &nstack ) {
  while (worklist.size() != 0) {
    Node *n = worklist.pop();
    // Only visit once
    if (visited.test_set(n->_idx)) continue;
    uint cnt = n->outcnt();
    uint   i = 0;
    while (true) {
      assert( _nodes[n->_idx], "no dead nodes" );
      // Visit all children
      if (i < cnt) {
        Node* use = n->raw_out(i);
        ++i;
        // Check for dead uses.  Aggressively prune such junk.  It might be
        // dead in the global sense, but still have local uses so I cannot
        // easily call 'remove_dead_node'.
        if( _nodes[use->_idx] != NULL || use->is_top() ) { // Not dead?
          // Due to cycles, we might not hit the same fixed point in the verify
          // pass as we do in the regular pass.  Instead, visit such phis as
          // simple uses of the loop head.
          if( use->in(0) && (use->is_CFG() || use->is_Phi()) ) {
            if( !visited.test(use->_idx) )
              worklist.push(use);
          } else if( !visited.test_set(use->_idx) ) {
            nstack.push(n, i); // Save parent and next use's index.
            n   = use;         // Process all children of current use.
            cnt = use->outcnt();
            i   = 0;
          }
        } else {
          // Do not visit around the backedge of loops via data edges.
          // push dead code onto a worklist
          _deadlist.push(use);
        }
      } else {
        // All of n's children have been processed, complete post-processing.
        build_loop_late_post(n);
        if (nstack.is_empty()) {
          // Finished all nodes on stack.
          // Process next node on the worklist.
          break;
        }
        // Get saved parent node and next use's index. Visit the rest of uses.
        n   = nstack.node();
        cnt = n->outcnt();
        i   = nstack.index();
        nstack.pop();
      }
    }
  }
}

//------------------------------build_loop_late_post---------------------------
// Put Data nodes into some loop nest, by setting the _nodes[]->loop mapping.
// Second pass finds latest legal placement, and ideal loop placement.
void PhaseIdealLoop::build_loop_late_post( Node *n ) {

  if (n->req() == 2 && n->Opcode() == Op_ConvI2L && !C->major_progress() && !_verify_only) {
    _igvn._worklist.push(n);  // Maybe we'll normalize it, if no more loops.
  }

  // CFG and pinned nodes already handled
  if( n->in(0) ) {
    if( n->in(0)->is_top() ) return; // Dead?

    // We'd like +VerifyLoopOptimizations to not believe that Mod's/Loads
    // _must_ be pinned (they have to observe their control edge of course).
    // Unlike Stores (which modify an unallocable resource, the memory
    // state), Mods/Loads can float around.  So free them up.
    bool pinned = true;
    switch( n->Opcode() ) {
    case Op_DivI:
    case Op_DivF:
    case Op_DivD:
    case Op_ModI:
    case Op_ModF:
    case Op_ModD:
    case Op_LoadB:              // Same with Loads; they can sink
    case Op_LoadUS:             // during loop optimizations.
    case Op_LoadD:
    case Op_LoadF:
    case Op_LoadI:
    case Op_LoadKlass:
    case Op_LoadNKlass:
    case Op_LoadL:
    case Op_LoadS:
    case Op_LoadP:
    case Op_LoadN:
    case Op_LoadRange:
    case Op_LoadD_unaligned:
    case Op_LoadL_unaligned:
    case Op_StrComp:            // Does a bunch of load-like effects
    case Op_StrEquals:
    case Op_StrIndexOf:
    case Op_AryEq:
      pinned = false;
    }
    if( pinned ) {
      IdealLoopTree *chosen_loop = get_loop(n->is_CFG() ? n : get_ctrl(n));
      if( !chosen_loop->_child )       // Inner loop?
        chosen_loop->_body.push(n); // Collect inner loops
      return;
    }
  } else {                      // No slot zero
    if( n->is_CFG() ) {         // CFG with no slot 0 is dead
      _nodes.map(n->_idx,0);    // No block setting, it's globally dead
      return;
    }
    assert(!n->is_CFG() || n->outcnt() == 0, "");
  }

  // Do I have a "safe range" I can select over?
  Node *early = get_ctrl(n);// Early location already computed

  // Compute latest point this Node can go
  Node *LCA = get_late_ctrl( n, early );
  // LCA is NULL due to uses being dead
  if( LCA == NULL ) {
#ifdef ASSERT
    for (DUIterator i1 = n->outs(); n->has_out(i1); i1++) {
      assert( _nodes[n->out(i1)->_idx] == NULL, "all uses must also be dead");
    }
#endif
    _nodes.map(n->_idx, 0);     // This node is useless
    _deadlist.push(n);
    return;
  }
  assert(LCA != NULL && !LCA->is_top(), "no dead nodes");

  Node *legal = LCA;            // Walk 'legal' up the IDOM chain
  Node *least = legal;          // Best legal position so far
  while( early != legal ) {     // While not at earliest legal
    // Find least loop nesting depth
    legal = idom(legal);        // Bump up the IDOM tree
    // Check for lower nesting depth
    if( get_loop(legal)->_nest < get_loop(least)->_nest )
      least = legal;
  }
  assert(early == legal || legal != C->root(), "bad dominance of inputs");

  // Try not to place code on a loop entry projection
  // which can inhibit range check elimination.
  if (least != early) {
    Node* ctrl_out = least->unique_ctrl_out();
    if (ctrl_out && ctrl_out->is_CountedLoop() &&
        least == ctrl_out->in(LoopNode::EntryControl)) {
      Node* least_dom = idom(least);
      if (get_loop(least_dom)->is_member(get_loop(least))) {
        least = least_dom;
      }
    }
  }

#ifdef ASSERT
  // If verifying, verify that 'verify_me' has a legal location
  // and choose it as our location.
  if( _verify_me ) {
    Node *v_ctrl = _verify_me->get_ctrl_no_update(n);
    Node *legal = LCA;
    while( early != legal ) {   // While not at earliest legal
      if( legal == v_ctrl ) break;  // Check for prior good location
      legal = idom(legal)      ;// Bump up the IDOM tree
    }
    // Check for prior good location
    if( legal == v_ctrl ) least = legal; // Keep prior if found
  }
#endif

  // Assign discovered "here or above" point
  least = find_non_split_ctrl(least);
  set_ctrl(n, least);

  // Collect inner loop bodies
  IdealLoopTree *chosen_loop = get_loop(least);
  if( !chosen_loop->_child )   // Inner loop?
    chosen_loop->_body.push(n);// Collect inner loops
}

#ifndef PRODUCT
//------------------------------dump-------------------------------------------
void PhaseIdealLoop::dump( ) const {
  ResourceMark rm;
  Arena* arena = Thread::current()->resource_area();
  Node_Stack stack(arena, C->unique() >> 2);
  Node_List rpo_list;
  VectorSet visited(arena);
  visited.set(C->top()->_idx);
  rpo( C->root(), stack, visited, rpo_list );
  // Dump root loop indexed by last element in PO order
  dump( _ltree_root, rpo_list.size(), rpo_list );
}

void PhaseIdealLoop::dump( IdealLoopTree *loop, uint idx, Node_List &rpo_list ) const {
  loop->dump_head();

  // Now scan for CFG nodes in the same loop
  for( uint j=idx; j > 0;  j-- ) {
    Node *n = rpo_list[j-1];
    if( !_nodes[n->_idx] )      // Skip dead nodes
      continue;
    if( get_loop(n) != loop ) { // Wrong loop nest
      if( get_loop(n)->_head == n &&    // Found nested loop?
          get_loop(n)->_parent == loop )
        dump(get_loop(n),rpo_list.size(),rpo_list);     // Print it nested-ly
      continue;
    }

    // Dump controlling node
    for( uint x = 0; x < loop->_nest; x++ )
      tty->print("  ");
    tty->print("C");
    if( n == C->root() ) {
      n->dump();
    } else {
      Node* cached_idom   = idom_no_update(n);
      Node *computed_idom = n->in(0);
      if( n->is_Region() ) {
        computed_idom = compute_idom(n);
        // computed_idom() will return n->in(0) when idom(n) is an IfNode (or
        // any MultiBranch ctrl node), so apply a similar transform to
        // the cached idom returned from idom_no_update.
        cached_idom = find_non_split_ctrl(cached_idom);
      }
      tty->print(" ID:%d",computed_idom->_idx);
      n->dump();
      if( cached_idom != computed_idom ) {
        tty->print_cr("*** BROKEN IDOM!  Computed as: %d, cached as: %d",
                      computed_idom->_idx, cached_idom->_idx);
      }
    }
    // Dump nodes it controls
    for( uint k = 0; k < _nodes.Size(); k++ ) {
      // (k < C->unique() && get_ctrl(find(k)) == n)
      if (k < C->unique() && _nodes[k] == (Node*)((intptr_t)n + 1)) {
        Node *m = C->root()->find(k);
        if( m && m->outcnt() > 0 ) {
          if (!(has_ctrl(m) && get_ctrl_no_update(m) == n)) {
            tty->print_cr("*** BROKEN CTRL ACCESSOR!  _nodes[k] is %p, ctrl is %p",
                          _nodes[k], has_ctrl(m) ? get_ctrl_no_update(m) : NULL);
          }
          for( uint j = 0; j < loop->_nest; j++ )
            tty->print("  ");
          tty->print(" ");
          m->dump();
        }
      }
    }
  }
}

// Collect a R-P-O for the whole CFG.
// Result list is in post-order (scan backwards for RPO)
void PhaseIdealLoop::rpo( Node *start, Node_Stack &stk, VectorSet &visited, Node_List &rpo_list ) const {
  stk.push(start, 0);
  visited.set(start->_idx);

  while (stk.is_nonempty()) {
    Node* m   = stk.node();
    uint  idx = stk.index();
    if (idx < m->outcnt()) {
      stk.set_index(idx + 1);
      Node* n = m->raw_out(idx);
      if (n->is_CFG() && !visited.test_set(n->_idx)) {
        stk.push(n, 0);
      }
    } else {
      rpo_list.push(m);
      stk.pop();
    }
  }
}
#endif


//=============================================================================
//------------------------------LoopTreeIterator-----------------------------------

// Advance to next loop tree using a preorder, left-to-right traversal.
void LoopTreeIterator::next() {
  assert(!done(), "must not be done.");
  if (_curnt->_child != NULL) {
    _curnt = _curnt->_child;
  } else if (_curnt->_next != NULL) {
    _curnt = _curnt->_next;
  } else {
    while (_curnt != _root && _curnt->_next == NULL) {
      _curnt = _curnt->_parent;
    }
    if (_curnt == _root) {
      _curnt = NULL;
      assert(done(), "must be done.");
    } else {
      assert(_curnt->_next != NULL, "must be more to do");
      _curnt = _curnt->_next;
    }
  }
}
