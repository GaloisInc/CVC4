/*********************                                                        */
/*! \file ho_trigger.cpp
 ** \verbatim
 ** Top contributors (to current version):
 **   Andrew Reynolds
 ** This file is part of the CVC4 project.
 ** Copyright (c) 2009-2017 by the authors listed in the file AUTHORS
 ** in the top-level source directory) and their institutional affiliations.
 ** All rights reserved.  See the file COPYING in the top-level source
 ** directory for licensing information.\endverbatim
 **
 ** \brief Implementation of higher-order trigger class
 ** 
 ** This class implements higher-order matching, examples (f, x, y are universal variables):
 ** 
 ** (f x y) matches (k 0 1) with possible solutions:
 ** 
 ** f -> \ xy. (k x y), x -> 0, y -> 1
 ** f -> \ xy. (k 0 y), x -> 0, y -> 1
 ** f -> \ xy. (k x 1), x -> 0, y -> 1
 ** f -> \ xy. (k 0 1), x -> 0, y -> 1
 ** 
 ** (f x y) matches (k 0 0) with possible solutions:
 ** 
 ** f -> \ xy. (k x x), x -> 0, y -> 0
 ** f -> \ xy. (k y x), x -> 0, y -> 0
 ** f -> \ xy. (k 0 x), x -> 0, y -> 0
 ** f -> \ xy. (k x y), x -> 0, y -> 0
 ** f -> \ xy. (k y y), x -> 0, y -> 0
 ** f -> \ xy. (k 0 y), x -> 0, y -> 0
 ** f -> \ xy. (k x 0), x -> 0, y -> 0
 ** f -> \ xy. (k y 0), x -> 0, y -> 0
 ** f -> \ xy. (k 0 0), x -> 0, y -> 0
 ** 
 ** (f x y), (f x z) simultaneously match (k 0 1), (k 0 2) with possible solutions:
 ** 
 ** f -> \ xy. (k x y), x -> 0, y -> 1, z -> 2
 ** f -> \ xy. (k 0 y), x -> 0, y -> 1, z -> 2
 ** 
 ** It also implements a way of forcing APPLY_UF to expand to curried HO_APPLY to
 ** handle a corner case where matching is stuck (addHoTypeMatchPredicateLemmas).
 ** 
 **/

#include "theory/quantifiers/ho_trigger.h"
#include "theory/quantifiers/term_database.h"
#include "theory/quantifiers_engine.h"
#include "theory/theory_engine.h"
#include "theory/uf/equality_engine.h"
#include "theory/uf/theory_uf_rewriter.h"
#include "util/hash.h"

using namespace std;
using namespace CVC4::kind;
using namespace CVC4::context;

namespace CVC4 {
namespace theory {
namespace inst {

HigherOrderTrigger::HigherOrderTrigger( QuantifiersEngine* qe, Node q, std::vector< Node >& nodes, 
                                        std::map< Node, std::vector< Node > >& ho_apps ) :
Trigger( qe, q, nodes ), d_ho_var_apps( ho_apps ) {
  // process the higher-order variable applications
  for( std::map< Node, std::vector< Node > >::iterator it = d_ho_var_apps.begin(); 
       it != d_ho_var_apps.end(); ++it ){
    Node n = it->first;
    d_ho_var_list.push_back( n );
    TypeNode tn = n.getType();
    Trace("ho-quant-trigger") << "  have " << it->second.size() << " patterns with variable operator " << n << std::endl;
    if( std::find( d_ho_var_types.begin(), d_ho_var_types.end(), tn )==d_ho_var_types.end() ){
      Trace("ho-quant-trigger") << "  type " << tn << " needs higher-order matching." << std::endl;
      d_ho_var_types.push_back( tn );
    }
    // make the bound variable lists
    qe->getTermDatabase()->getLambdaArgs( n, d_ho_var_bvs[n] );
    Assert( !d_ho_var_bvs[n].empty() );
    d_ho_var_bvl[n] = NodeManager::currentNM()->mkNode( kind::BOUND_VAR_LIST, d_ho_var_bvs[n] );
  }
}

HigherOrderTrigger::~HigherOrderTrigger() {

}

void HigherOrderTrigger::collectHoVarApplyTerms( Node q, TNode n, std::map< Node, std::vector< Node > >& apps, 
                                                 std::map< TNode, bool >& visited, bool withinApply ) {
  std::map< TNode, bool >::iterator itv = visited.find( n );
  if( itv==visited.end() ){
    visited[n] = true;
    if( !withinApply && n.getKind()==HO_APPLY ){
      TNode curr = n;
      while( curr.getKind() == kind::HO_APPLY ){
        curr = curr[0];        
      }
      if( curr.getKind()==INST_CONSTANT ){
        Assert( quantifiers::TermDb::getInstConstAttr(n)==q );
        Trace("ho-quant-trigger-debug") << "Ho variable apply term : " << n << " with head " << curr << std::endl;
        apps[curr].push_back( n );
      }
    }
    for( unsigned i=0; i<n.getNumChildren(); i++ ){
      bool newWithinApply = !withinApply || i!=0;
      collectHoVarApplyTerms( q, n[i], apps, visited, newWithinApply );
    }
  }
}

void HigherOrderTrigger::collectHoVarApplyTerms( Node q, TNode n, std::map< Node, std::vector< Node > >& apps ) {
  std::map< TNode, bool > visited;
  collectHoVarApplyTerms( q, n, apps, visited, false );
}

int HigherOrderTrigger::addInstantiations( InstMatch& baseMatch ){
  int addedFoLemmas = addBasicInstantiations( baseMatch );
  int addedHoLemmas = addHoTypeMatchPredicateLemmas();
  return addedHoLemmas+addedFoLemmas;
}

bool HigherOrderTrigger::sendInstantiation( InstMatch& m ) {
  if( options::hoMatching() ){
    // get substitution corresponding to m
    std::vector< TNode > vars;
    std::vector< TNode > subs;
    for( unsigned i=0; i<d_f[0].getNumChildren(); i++ ){
      subs.push_back( m.d_vals[i] );  
    }
    std::map< Node, std::vector< Node > >::iterator itic = d_quantEngine->getTermDatabase()->d_inst_constants.find( d_f );
    Assert( itic!=d_quantEngine->getTermDatabase()->d_inst_constants.end() );
    vars.insert( vars.end(), itic->second.begin(), itic->second.end() );
    Assert( vars.size()==subs.size() );

    Trace("ho-unif-debug") << "Run higher-order unification..." << std::endl;

    // get the substituted form of all variable-operator ho application terms
    std::map< TNode, std::vector< Node > > ho_var_apps_subs;
    for( std::map< Node, std::vector< Node > >::iterator it = d_ho_var_apps.begin(); 
         it != d_ho_var_apps.end(); ++it ){
      TNode var = it->first;
      for( unsigned j=0; j<it->second.size(); j++ ){
        TNode app = it->second[j];
        Node sapp = app.substitute( vars.begin(), vars.end(), subs.begin(), subs.end() );
        ho_var_apps_subs[var].push_back( sapp );
        Trace("ho-unif-debug") << "  app[" << var << "] : " << app << " -> " << sapp << std::endl;
      }  
    }

    // compute argument vectors for each variable
    d_lchildren.clear();
    d_arg_to_arg_rep.clear();
    d_arg_vector.clear();
    EqualityQuery* eq = d_quantEngine->getEqualityQuery();
    for( std::map< TNode, std::vector< Node > >::iterator ith = ho_var_apps_subs.begin();
         ith != ho_var_apps_subs.end(); ++ith ){
      TNode var = ith->first;
      unsigned vnum = var.getAttribute(InstVarNumAttribute());
      Node value = m.d_vals[vnum];
      Trace("ho-unif-debug") << "  val[" << var << "] = " << value << std::endl;

      Trace("ho-unif-debug2") << "initialize lambda information..." << std::endl;
      // initialize the lambda children
      d_lchildren[vnum].push_back( value );
      std::map< TNode, std::vector< Node > >::iterator ithb = d_ho_var_bvs.find( var );
      Assert( ithb!=d_ho_var_bvs.end() );
      d_lchildren[vnum].insert( d_lchildren[vnum].end(), ithb->second.begin(), ithb->second.end() );

      Trace("ho-unif-debug2") << "compute fixed arguments..." << std::endl;
      // compute for each argument if it is only applied to a fixed value modulo equality
      std::map< unsigned, Node > fixed_vals;
      for( unsigned i=0; i<ith->second.size(); i++ ){
        std::vector<TNode> args;
        Node f = uf::TheoryUfRewriter::decomposeHoApply( ith->second[i], args );
        //Assert( f==value );
        for( unsigned k=0; k<args.size(); k++ ){
          Node val = args[k];
          std::map< unsigned, Node >::iterator itf = fixed_vals.find( k );
          if( itf==fixed_vals.end() ){
            fixed_vals[k] = val;
          }else if( !itf->second.isNull() ){
            if( !eq->areEqual( itf->second, args[k] ) ){
              fixed_vals[k] = Node::null();
            }
          }
        }
      }
      if( Trace.isOn("ho-unif-debug") ){
        for( std::map< unsigned, Node >::iterator itf = fixed_vals.begin(); itf != fixed_vals.end(); ++itf ){
          Trace("ho-unif-debug") << "  arg[" << var << "][" << itf->first << "] : " << itf->second << std::endl;
        }
      }

      // now construct argument vectors
      Trace("ho-unif-debug2") << "compute argument vectors..." << std::endl;
      std::map< Node, unsigned > arg_to_rep;
      for( unsigned index=0; index<ithb->second.size(); index++ ){
        Node bv_at_index = ithb->second[index];
        std::map< unsigned, Node >::iterator itf = fixed_vals.find( index );
        Trace("ho-unif-debug") << "  * arg[" << var << "][" << index << "]";
        if( itf!=fixed_vals.end() ){
          if( !itf->second.isNull() ){
            Node r = eq->getRepresentative( itf->second );
            std::map< Node, unsigned >::iterator itfr = arg_to_rep.find( r );
            if( itfr!=arg_to_rep.end() ){
              d_arg_to_arg_rep[vnum][index] = itfr->second;
              // function applied to equivalent values at multiple arguments, can permute variables
              d_arg_vector[vnum][itfr->second].push_back( bv_at_index );
              Trace("ho-unif-debug") << " = { self } ++ arg[" << var << "][" << itfr->second << "]" << std::endl;
            }else{
              arg_to_rep[r] = index;
              // function applied to single value, can either use variable or value at this argument position
              d_arg_vector[vnum][index].push_back( itf->second );
              d_arg_vector[vnum][index].push_back( bv_at_index );
              Trace("ho-unif-debug") << " = { " << itf->second << ", self } " << std::endl;
            }
          }else{
            // function is applied to disequal values, can only use variable at this argument position
            d_arg_vector[vnum][index].push_back( bv_at_index );
              Trace("ho-unif-debug") << " = { self } (disequal)" << std::endl;
          }
        }else{
          // argument is irrelevant to matching, assume identity variable
          d_arg_vector[vnum][index].push_back( bv_at_index );
          Trace("ho-unif-debug") << " = { self } (irrelevant)" << std::endl;
        }
      }
      Trace("ho-unif-debug2") << "finished." << std::endl;
    }

    return sendInstantiation( m, 0 );
  }else{
    // do not run higher-order matching
    return d_quantEngine->addInstantiation( d_f, m );
  }
}

// recursion depth limited by number of arguments of higher order variables occurring as pattern operators (very small)
bool HigherOrderTrigger::sendInstantiation( InstMatch& m, unsigned var_index ) {
  if( var_index==d_ho_var_list.size() ){
    // we now have an instantiation to try
    return d_quantEngine->addInstantiation( d_f, m );
  }else{
    Node var = d_ho_var_list[var_index];
    unsigned vnum = var.getAttribute(InstVarNumAttribute());
    Assert( vnum<m.d_vals.size() );
    Node value = m.d_vals[vnum];
    Assert( d_lchildren[vnum][0] == value );
    Assert( d_ho_var_bvl.find( var )!=d_ho_var_bvl.end() );

    // now, recurse on arguments to enumerate equivalent matching lambda expressions
    bool ret = sendInstantiationArg( m, var_index, vnum, 0, d_ho_var_bvl[var], false );

    //reset the value
    m.d_vals[vnum] = value;
    
    return ret;
  }
}

bool HigherOrderTrigger::sendInstantiationArg( InstMatch& m, unsigned var_index, unsigned vnum, unsigned arg_index,
                                               Node lbvl, bool arg_changed ) {
  
  if( arg_index==lbvl.getNumChildren() ){
    // construct the lambda
    if( arg_changed ){
      Node body = NodeManager::currentNM()->mkNode( kind::APPLY_UF, d_lchildren[vnum] );
      Node lam = NodeManager::currentNM()->mkNode( kind::LAMBDA, lbvl, body );
      m.d_vals[vnum] = lam;
      Trace("ho-unif-debug2") << "  try " << vnum << " -> " << lam << std::endl;
    }
    return sendInstantiation( m, var_index+1 );
  }else{
    std::map< unsigned, unsigned >::iterator itr = d_arg_to_arg_rep[vnum].find( arg_index );
    unsigned rindex = itr!=d_arg_to_arg_rep[vnum].end() ? itr->second : arg_index;
    std::map< unsigned, std::vector< Node > >::iterator itv = d_arg_vector[vnum].find( rindex );
    Assert( itv!=d_arg_vector[vnum].end() );
    Node prev = lbvl[arg_index];
    bool ret = false;
    // try each argument in the vector
    for( unsigned i=0; i<itv->second.size(); i++ ){
      bool new_arg_changed = arg_changed || prev!=itv->second[i];
      d_lchildren[vnum][arg_index+1] = itv->second[i];
      if( sendInstantiationArg( m, var_index, vnum, arg_index+1, lbvl, new_arg_changed ) ){
        ret = true;
        break;
      }
    }
    // clean up
    d_lchildren[vnum][arg_index+1] = prev;
    return ret;
  }
}

int HigherOrderTrigger::addHoTypeMatchPredicateLemmas() {
  unsigned numLemmas = 0;
  if( !d_ho_var_types.empty() ){
    // this forces expansion of APPLY_UF terms to curried HO_APPLY chains
    for( std::map< Node, std::vector< Node > >::iterator it = d_quantEngine->getTermDatabase()->d_op_map.begin(); 
         it != d_quantEngine->getTermDatabase()->d_op_map.end(); ++it ){
      if( it->first.isVar() ){
        TypeNode tn = it->first.getType();
        if( std::find( d_ho_var_types.begin(), d_ho_var_types.end(), tn )!=d_ho_var_types.end() ){
          Node u = d_quantEngine->getTermDatabase()->getHoTypeMatchPredicate( tn );
          Node au = NodeManager::currentNM()->mkNode( kind::APPLY_UF, u, it->first );
          if( d_quantEngine->addLemma( au ) ){
            //this forces it->first to be a first-class member of the quantifier-free equality engine,
            //  which in turn forces the quantifier-free theory solver to expand it to HO_APPLY
            Trace("ho-quant") << "Added ho match predicate lemma : " << au << std::endl;
            numLemmas++;
          }
        }
      }
    }
  }
  return numLemmas;
}

}/* CVC4::theory::inst namespace */
}/* CVC4::theory namespace */
}/* CVC4 namespace */
