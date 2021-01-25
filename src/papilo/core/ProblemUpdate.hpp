/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*               This file is part of the program and library                */
/*    PaPILO --- Parallel Presolve for Integer and Linear Optimization       */
/*                                                                           */
/* Copyright (C) 2020-2021 Konrad-Zuse-Zentrum                               */
/*                     fuer Informationstechnik Berlin                       */
/*                                                                           */
/* This program is free software: you can redistribute it and/or modify      */
/* it under the terms of the GNU Lesser General Public License as published  */
/* by the Free Software Foundation, either version 3 of the License, or      */
/* (at your option) any later version.                                       */
/*                                                                           */
/* This program is distributed in the hope that it will be useful,           */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of            */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the             */
/* GNU Lesser General Public License for more details.                       */
/*                                                                           */
/* You should have received a copy of the GNU Lesser General Public License  */
/* along with this program.  If not, see <https://www.gnu.org/licenses/>.    */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifndef _PAPILO_CORE_PROBLEM_UPDATE_HPP_
#define _PAPILO_CORE_PROBLEM_UPDATE_HPP_

#include "papilo/core/MatrixBuffer.hpp"
#include "papilo/core/Postsolve.hpp"
#include "papilo/core/PresolveMethod.hpp"
#include "papilo/core/PresolveOptions.hpp"
#include "papilo/core/Problem.hpp"
#include "papilo/core/Reductions.hpp"
#include "papilo/core/SingleRow.hpp"
#include "papilo/core/Statistics.hpp"
#include "papilo/misc/Flags.hpp"
#include "papilo/misc/MultiPrecision.hpp"
#include "papilo/misc/Num.hpp"
#include <cstdint>
#include <random>

namespace papilo
{

enum class ConflictType
{
   kNoConflict,
   kConflict,
   kPostpone
};

enum class ApplyResult
{
   kApplied,
   kRejected,
   kPostponed,
   kInfeasible
};

template <typename REAL>
class ProblemUpdate
{
   Problem<REAL>& problem;
   Postsolve<REAL>& postsolve;
   Statistics& stats;
   const PresolveOptions& presolveOptions;
   const Num<REAL>& num;

   bool postponeSubstitutions;
   Vec<int> dirty_row_states;
   Vec<int> dirty_col_states;

   Vec<int> deleted_cols;
   Vec<int> redundant_rows;

   Vec<int> changed_activities;
   Vec<int> singletonRows;
   Vec<int> singletonColumns;
   Vec<int> emptyColumns;
   int firstNewSingletonCol;

   MatrixBuffer<REAL> matrix_buffer;
   Vec<int> intbuffer;
   Vec<REAL> realbuffer;
   Vec<Triplet<REAL>> tripletbuffer;

   Vec<PresolveMethod<REAL>*> compress_observers;

   Vec<int> random_col_perm;
   Vec<int> random_row_perm;

   int lastcompress_ndelcols;
   int lastcompress_ndelrows;

   enum class State : uint8_t
   {
      kUnmodified = 0,
      kLocked = 1 << 0,
      kModified = 1 << 1,
      kBoundsModified = 1 << 2,
   };

   Vec<Flags<State>> row_state;
   Vec<Flags<State>> col_state;

   template <typename... Args>
   void
   setColState( int col, Args... flags )
   {
      assert( col >= 0 && col < problem.getNCols() );

      if( col_state[col].equal( State::kUnmodified ) )
         dirty_col_states.push_back( col );

      col_state[col].set( flags... );
   }

   template <typename... Args>
   void
   setRowState( int row, Args... flags )
   {
      assert( row >= 0 && row < problem.getNRows() );

      // check that equation flag is set correctly
      assert(
          problem.getRowFlags()[row].test( RowFlag::kRedundant ) ||
          ( !problem.getRowFlags()[row].test( RowFlag::kEquation ) &&
            ( problem.getRowFlags()[row].test( RowFlag::kLhsInf,
                                               RowFlag::kRhsInf ) ||
              problem.getConstraintMatrix().getLeftHandSides()[row] !=
                  problem.getConstraintMatrix().getRightHandSides()[row] ) ) ||
          ( problem.getRowFlags()[row].test( RowFlag::kEquation ) &&
            !problem.getRowFlags()[row].test( RowFlag::kLhsInf,
                                              RowFlag::kRhsInf ) &&
            problem.getConstraintMatrix().getLeftHandSides()[row] ==
                problem.getConstraintMatrix().getRightHandSides()[row] ) );

      if( row_state[row].equal( State::kUnmodified ) )
         dirty_row_states.push_back( row );

      row_state[row].set( flags... );
   }

 public:
   ProblemUpdate( Problem<REAL>& problem, Postsolve<REAL>& postsolve,
                  Statistics& stats, const PresolveOptions& presolveOptions,
                  const Num<REAL>& num );

   void
   setPostponeSubstitutions( bool postponeSubstitutions )
   {
      this->postponeSubstitutions = postponeSubstitutions;
   }

   void
   update_activity( ActivityChange actChange, int rowid,
                    RowActivity<REAL>& activity );

   PresolveStatus
   fixCol( int col, REAL val );

   PresolveStatus
   fixColInfinity( int col, REAL val );

   PresolveStatus
   changeLB( int col, REAL val );

   PresolveStatus
   changeUB( int col, REAL val );

   void
   markRowRedundant( int row )
   {
      RowFlags& rflags = problem.getRowFlags()[row];
      if( !rflags.test( RowFlag::kRedundant ) )
      {
         redundant_rows.push_back( row );
         ++stats.ndeletedrows;
         rflags.set( RowFlag::kRedundant );
      }
   }

   void
   observeCompress( PresolveMethod<REAL>* observer )
   {
      compress_observers.push_back( observer );
   }

   void
   markColFixed( int col )
   {
      ColFlags& cflags = problem.getColFlags()[col];
      assert( !cflags.test( ColFlag::kInactive ) );
      cflags.set( ColFlag::kFixed );
      deleted_cols.push_back( col );
      ++stats.ndeletedcols;

      if( cflags.test( ColFlag::kIntegral ) )
         --problem.getNumIntegralCols();
      else
         --problem.getNumContinuousCols();
   }

   /// removes the constant contribution of fixed columns
   /// from the left and right hand sides and the activity
   void
   removeFixedCols();

   /// scans through all columns and does some trivial presolve reductions:
   ///   * it rounds fractional bounds of integer variables
   ///   * checks for infeasibility of bound constraints
   ///   * performs dual fixing if the parameter dualfix is true
   ///   * remembers singleton columns
   /// if dualfixing is true the locks of the problem must have been initialized
   /// by calling problem.recomputeLocks()
   PresolveStatus
   trivialColumnPresolve();

   /// scans through all rows and does some trivial presolve reductions:
   ///   * removes singleton rows and updates the column bounds accordingly
   ///   * checks if the row is redundant or proves infeasibility w.r.t. the
   ///   activity bounds
   /// The activities of the problem must be initialized before calling this
   /// fucntion by callnig problem.recomputeActivities()
   PresolveStatus
   trivialRowPresolve();

   /// performs trivial row and column presolve and initializes the locks and
   /// acitivities. Updates the matrix to reflect the changes
   PresolveStatus
   trivialPresolve();

   /// adds a singleton row as a bound change and marks the row redundant
   PresolveStatus
   removeSingletonRow( int row );

   /// cleanup small coefficients from single row, adds coefficients changes to
   /// the matrix buffer
   void
   cleanupSmallCoefficients( int row );

   PresolveStatus
   removeEmptyColumns();

   void
   compress( bool full = false );

   /// check changed activities for infeasibility and row redundancy
   PresolveStatus
   checkChangedActivities();

   /// flush changes after applying several reductions
   PresolveStatus
   flush();

   /// flush changes coefficients after applying several reductions
   void
   flushChangedCoeffs();

   void
   clearChangeInfo()
   {
      changed_activities.clear();
      firstNewSingletonCol = singletonColumns.size();
   }

   void
   clearStates();

   const Vec<int>&
   getChangedActivities() const
   {
      return changed_activities;
   }

   const Vec<int>&
   getSingletonCols() const
   {
      return singletonColumns;
   }

   const Vec<int>&
   getRandomColPerm() const
   {
      return random_col_perm;
   }

   const Vec<int>&
   getRandomRowPerm() const
   {
      return random_row_perm;
   }

   bool
   isColBetterForSubstitution( int col1, int col2 ) const
   {
      int col1size = problem.getColSizes()[col1];
      int col2size = problem.getColSizes()[col2];

      // first criterion is sparsity
      if( col1size < col2size )
         return true;
      if( col2size < col1size )
         return false;

      // second criterion is whether the objective is zero
      bool obj1zero = problem.getObjective().coefficients[col1] == 0;
      bool obj2zero = problem.getObjective().coefficients[col2] == 0;

      if( obj1zero && !obj2zero )
         return true;
      if( !obj1zero && obj2zero )
         return false;

      // tie breaker is the random column permutation
      return random_col_perm[col1] < random_col_perm[col2];
   }

   int
   getFirstNewSingletonCol() const
   {
      return firstNewSingletonCol;
   }

   int
   getNActiveRows() const
   {
      return problem.getNRows() - stats.ndeletedrows + lastcompress_ndelrows;
   }

   int
   getNActiveCols() const
   {
      return problem.getNCols() - stats.ndeletedcols + lastcompress_ndelcols;
   }

   const PresolveOptions&
   getPresolveOptions() const
   {
      return presolveOptions;
   }

   std::pair<int, int>
   removeRedundantBounds()
   {
      return problem.removeRedundantBounds( num, problem.getColFlags(),
                                            problem.getRowActivities() );
   }

   /// returns true if the given transaction conflicts with the current state of
   /// changes and false otherwise
   ConflictType
   checkTransactionConflicts( const Reduction<REAL>* first,
                              const Reduction<REAL>* last );

   /// returns true if the given transaction was applied and false otherwise
   ApplyResult
   applyTransaction( const Reduction<REAL>* first,
                     const Reduction<REAL>* last );
   void
   roundIntegralColumns( Vec<REAL>& lbs, Vec<REAL>& ubs, int col,
                         Vec<ColFlags>& cflags, PresolveStatus& status );
   void
   mark_huge_values( const Vec<REAL>& lbs, const Vec<REAL>& ubs,
                     Vec<ColFlags>& cflags, int col );
   bool
   is_dualfix_enabled( const Vec<REAL>& obj, int col ) const;

   PresolveStatus
   apply_dualfix( Vec<REAL>& lbs, Vec<REAL>& ubs, Vec<ColFlags>& cflags,
                  const Vec<REAL>& obj, const Vec<Locks>& locks, int col );

};

#ifdef PAPILO_USE_EXTERN_TEMPLATES
extern template class ProblemUpdate<double>;
extern template class ProblemUpdate<Quad>;
extern template class ProblemUpdate<Rational>;
#endif

template <typename REAL>
ProblemUpdate<REAL>::ProblemUpdate( Problem<REAL>& problem,
                                    Postsolve<REAL>& postsolve,
                                    Statistics& stats,
                                    const PresolveOptions& presolveOptions,
                                    const Num<REAL>& num )
    : problem( problem ), postsolve( postsolve ), stats( stats ),
      presolveOptions( presolveOptions ), num( num )
{
   row_state.resize( problem.getNRows() );
   col_state.resize( problem.getNCols() );
   postponeSubstitutions = true;
   firstNewSingletonCol = 0;

   lastcompress_ndelcols = 0;
   lastcompress_ndelrows = 0;

   std::ranlux24 randgen( presolveOptions.randomseed );
   random_col_perm.resize( problem.getNCols() );
   for( int i = 0; i < problem.getNCols(); ++i )
      random_col_perm[i] = i;
   std::shuffle( random_col_perm.begin(), random_col_perm.end(), randgen );

   random_row_perm.resize( problem.getNRows() );
   for( int i = 0; i < problem.getNRows(); ++i )
      random_row_perm[i] = i;
   std::shuffle( random_row_perm.begin(), random_row_perm.end(), randgen );
}

template <typename REAL>
void
ProblemUpdate<REAL>::update_activity( ActivityChange actChange, int rowid,
                                      RowActivity<REAL>& activity )
{
   if( activity.lastchange == stats.nrounds )
      return;

   if( actChange == ActivityChange::kMin && activity.ninfmin > 1 )
      return;

   if( actChange == ActivityChange::kMax && activity.ninfmax > 1 )
      return;

   if( problem.getConstraintMatrix().isRowRedundant( rowid ) )
      return;

   activity.lastchange = stats.nrounds;

   changed_activities.push_back( rowid );
}

template <typename REAL>
PresolveStatus
ProblemUpdate<REAL>::fixCol( int col, REAL val )
{
   ConstraintMatrix<REAL>& constraintMatrix = problem.getConstraintMatrix();
   Vec<REAL>& lbs = problem.getLowerBounds();
   Vec<REAL>& ubs = problem.getUpperBounds();
   Vec<ColFlags>& cflags = problem.getColFlags();

   if( cflags[col].test( ColFlag::kSubstituted ) )
      return PresolveStatus::kUnchanged;

   auto updateActivity = [this]( ActivityChange actChange, int rowid,
                                 RowActivity<REAL>& activity ) {
      update_activity( actChange, rowid, activity );
   };

   bool lbchanged = cflags[col].test( ColFlag::kLbInf ) || val != lbs[col];
   bool ubchanged = cflags[col].test( ColFlag::kUbInf ) || val != ubs[col];

   if( lbchanged )
      ++stats.nboundchgs;
   if( ubchanged )
      ++stats.nboundchgs;

   if( lbchanged || ubchanged )
   {
      auto colvec = constraintMatrix.getColumnCoefficients( col );

      if( ( !cflags[col].test( ColFlag::kLbInf ) &&
            num.isFeasLT( val, lbs[col] ) ) ||
          ( !cflags[col].test( ColFlag::kUbInf ) &&
            num.isFeasGT( val, ubs[col] ) ) ||
          ( cflags[col].test( ColFlag::kIntegral ) &&
            !num.isFeasIntegral( val ) ) )
      {
         Message::debug( this,
                         "fixing {} col {} with bounds [{},{}] to value {} was "
                         "detected to be infeasible\n",
                         cflags[col].test( ColFlag::kIntegral ) ? "integral"
                                                                : "continuous",
                         col,
                         cflags[col].test( ColFlag::kLbInf )
                             ? -std::numeric_limits<double>::infinity()
                             : double( lbs[col] ),
                         cflags[col].test( ColFlag::kUbInf )
                             ? std::numeric_limits<double>::infinity()
                             : double( ubs[col] ),
                         double( val ) );
         return PresolveStatus::kInfeasible;
      }

      // TODO: should be this case be already checked before (the stats may also
      // be false)
      if( cflags[col].test( ColFlag::kFixed ) )
         return PresolveStatus::kUnchanged;

      if( lbchanged )
      {
         update_activities_after_boundchange(
             colvec.getValues(), colvec.getIndices(), colvec.getLength(),
             BoundChange::kLower, lbs[col], val,
             cflags[col].test( ColFlag::kLbUseless ),
             problem.getRowActivities(), updateActivity );

         lbs[col] = val;
         cflags[col].unset( ColFlag::kLbUseless );
      }

      if( ubchanged )
      {
         // TODO: warum benutzt man nicht die bereits vorhandenen Funktionen,
         // wie changeUB
         update_activities_after_boundchange(
             colvec.getValues(), colvec.getIndices(), colvec.getLength(),
             BoundChange::kUpper, ubs[col], val,
             cflags[col].test( ColFlag::kUbUseless ),
             problem.getRowActivities(), updateActivity );

         ubs[col] = val;
         cflags[col].unset( ColFlag::kUbUseless );
      }

      // remember fixed column
      markColFixed( col );

      setColState( col, State::kBoundsModified );

      return PresolveStatus::kReduced;
   }

   assert( cflags[col].test( ColFlag::kFixed ) );

   return PresolveStatus::kUnchanged;
}

template <typename REAL>
PresolveStatus
ProblemUpdate<REAL>::fixColInfinity( int col, REAL val )
{
   ConstraintMatrix<REAL>& constraintMatrix = problem.getConstraintMatrix();
   Vec<REAL>& lbs = problem.getLowerBounds();
   Vec<REAL>& ubs = problem.getUpperBounds();
   Vec<ColFlags>& cflags = problem.getColFlags();

   if( cflags[col].test( ColFlag::kSubstituted ) ||
       cflags[col].test( ColFlag::kFixed ) || val == 0 )
      return PresolveStatus::kUnchanged;

   assert(val < 0 && !cflags[col].test( ColFlag::kLbInf ) );
   assert(val > 0 && !cflags[col].test( ColFlag::kUbInf ) );

   // activity doesn't need to be upgraded because rows should be mark redundant
   markColFixed( col );

   setColState( col, State::kBoundsModified );

   return PresolveStatus::kReduced;
}

template <typename REAL>
PresolveStatus
ProblemUpdate<REAL>::changeLB( int col, REAL val )
{
   ConstraintMatrix<REAL>& constraintMatrix = problem.getConstraintMatrix();
   Vec<ColFlags>& cflags = problem.getColFlags();
   Vec<REAL>& lbs = problem.getLowerBounds();
   Vec<REAL>& ubs = problem.getUpperBounds();

   if( cflags[col].test( ColFlag::kSubstituted ) )
      return PresolveStatus::kUnchanged;

   REAL newbound = val;

   auto updateActivity = [this]( ActivityChange actChange, int rowid,
                                 RowActivity<REAL>& activity ) {
      update_activity( actChange, rowid, activity );
   };

   if( cflags[col].test( ColFlag::kIntegral, ColFlag::kImplInt ) )
      newbound = num.feasCeil( newbound );

   if( cflags[col].test( ColFlag::kLbInf ) || newbound > lbs[col] )
   {
      ++stats.nboundchgs;
      if( !cflags[col].test( ColFlag::kUbInf ) && newbound > ubs[col] )
      {
         if( num.isFeasGT( newbound, ubs[col] ) )
         {
            Message::debug( this,
                            "changing lower bound of {} col {} with bounds "
                            "[{},{}] to value {} was "
                            "detected to be infeasible\n",
                            cflags[col].test( ColFlag::kIntegral )
                                ? "integral"
                                : "continuous",
                            col,
                            cflags[col].test( ColFlag::kLbInf )
                                ? -std::numeric_limits<double>::infinity()
                                : double( lbs[col] ),
                            cflags[col].test( ColFlag::kUbInf )
                                ? std::numeric_limits<double>::infinity()
                                : double( ubs[col] ),
                            double( newbound ) );
            return PresolveStatus::kInfeasible;
         }

         if( !cflags[col].test( ColFlag::kLbInf ) && lbs[col] == ubs[col] )
            return PresolveStatus::kUnchanged;

         newbound = ubs[col];
      }

      if( !num.isHugeVal( newbound ) )
      {
         auto colvec = constraintMatrix.getColumnCoefficients( col );
         update_activities_after_boundchange(
             colvec.getValues(), colvec.getIndices(), colvec.getLength(),
             BoundChange::kLower, lbs[col], newbound,
             cflags[col].test( ColFlag::kLbUseless ),
             problem.getRowActivities(), updateActivity );

         cflags[col].unset( ColFlag::kLbUseless );
      }
      else
         cflags[col].unset( ColFlag::kLbInf );

      lbs[col] = newbound;

      if( !cflags[col].test( ColFlag::kUbInf ) && ubs[col] == lbs[col] )
      {
         cflags[col].set( ColFlag::kFixed );
         deleted_cols.push_back( col );
         ++stats.ndeletedcols;

         if( cflags[col].test( ColFlag::kIntegral ) )
            --problem.getNumIntegralCols();
         else
            --problem.getNumContinuousCols();
      }

      setColState( col, State::kBoundsModified );

      return PresolveStatus::kReduced;
   }

   return PresolveStatus::kUnchanged;
}

template <typename REAL>
PresolveStatus
ProblemUpdate<REAL>::changeUB( int col, REAL val )
{
   ConstraintMatrix<REAL>& constraintMatrix = problem.getConstraintMatrix();
   Vec<ColFlags>& cflags = problem.getColFlags();
   Vec<REAL>& lbs = problem.getLowerBounds();
   Vec<REAL>& ubs = problem.getUpperBounds();

   if( cflags[col].test( ColFlag::kSubstituted ) )
      return PresolveStatus::kUnchanged;

   REAL newbound = val;

   auto updateActivity = [this]( ActivityChange actChange, int rowid,
                                 RowActivity<REAL>& activity ) {
      update_activity( actChange, rowid, activity );
   };

   if( cflags[col].test( ColFlag::kIntegral, ColFlag::kImplInt ) )
      newbound = num.feasFloor( newbound );

   if( cflags[col].test( ColFlag::kUbInf ) || newbound < ubs[col] )
   {
      ++stats.nboundchgs;
      if( !cflags[col].test( ColFlag::kLbInf ) && newbound < lbs[col] )
      {
         if( num.isFeasLT( newbound, lbs[col] ) )
         {
            Message::debug( this,
                            "changing upper bound of {} col {} with bounds "
                            "[{},{}] to value {} was "
                            "detected to be infeasible\n",
                            cflags[col].test( ColFlag::kIntegral )
                                ? "integral"
                                : "continuous",
                            col,
                            cflags[col].test( ColFlag::kLbInf )
                                ? -std::numeric_limits<double>::infinity()
                                : double( lbs[col] ),
                            cflags[col].test( ColFlag::kUbInf )
                                ? std::numeric_limits<double>::infinity()
                                : double( ubs[col] ),
                            double( newbound ) );
            return PresolveStatus::kInfeasible;
         }

         if( !cflags[col].test( ColFlag::kUbInf ) && lbs[col] == ubs[col] )
            return PresolveStatus::kUnchanged;

         newbound = lbs[col];
      }

      if( !num.isHugeVal( newbound ) )
      {
         auto colvec = constraintMatrix.getColumnCoefficients( col );
         update_activities_after_boundchange(
             colvec.getValues(), colvec.getIndices(), colvec.getLength(),
             BoundChange::kUpper, ubs[col], newbound,
             cflags[col].test( ColFlag::kUbUseless ),
             problem.getRowActivities(), updateActivity );
         cflags[col].unset( ColFlag::kUbUseless );
      }
      else
         cflags[col].unset( ColFlag::kUbInf );

      ubs[col] = newbound;

      if( !cflags[col].test( ColFlag::kLbInf ) && ubs[col] == lbs[col] )
      {
         cflags[col].set( ColFlag::kFixed );
         deleted_cols.push_back( col );
         ++stats.ndeletedcols;

         if( cflags[col].test( ColFlag::kIntegral ) )
            --problem.getNumIntegralCols();
         else
            --problem.getNumContinuousCols();
      }

      setColState( col, State::kBoundsModified );

      return PresolveStatus::kReduced;
   }

   return PresolveStatus::kUnchanged;
}

template <typename REAL>
void
ProblemUpdate<REAL>::compress( bool full )
{
   if( problem.getNCols() == getNActiveCols() &&
       problem.getNRows() == getNActiveRows() && !full )
      return;

   Message::debug( this,
                   "compressing problem ({} rows, {} cols) to active problem "
                   "({} rows, {} cols)\n",
                   problem.getNRows(), problem.getNCols(), getNActiveRows(),
                   getNActiveCols() );

   std::pair<Vec<int>, Vec<int>> mappings = problem.compress( full );
   assert( redundant_rows.empty() );
   assert( deleted_cols.empty() );
   assert( dirty_col_states.empty() );
   assert( dirty_row_states.empty() );
   assert( matrix_buffer.empty() );

   row_state.resize( problem.getNRows() );
   col_state.resize( problem.getNCols() );

   tbb::parallel_invoke(
       [this, &mappings, full]() {
          compress_index_vector( mappings.first, random_row_perm );
          if( full )
             random_row_perm.shrink_to_fit();
       },
       [this, &mappings, full]() {
          compress_index_vector( mappings.second, random_col_perm );
          if( full )
             random_col_perm.shrink_to_fit();
       },
       [this, &mappings, full]() {
          postsolve.compress( mappings.first, mappings.second, full );
       },
       [this, &mappings, full]() {
          // update row index sets
          compress_index_vector( mappings.first, changed_activities );
          if( full )
             changed_activities.shrink_to_fit();
       },
       [this, &mappings, full]() {
          compress_index_vector( mappings.first, singletonRows );
          if( full )
             singletonRows.shrink_to_fit();
       },
       // update column index sets
       [this, &mappings, full]() {
          int numNewSingletonCols =
              static_cast<int>( singletonColumns.size() ) -
              firstNewSingletonCol;
          compress_index_vector( mappings.second, singletonColumns );
          firstNewSingletonCol =
              std::max( 0, static_cast<int>( singletonColumns.size() ) -
                               numNewSingletonCols );
          if( full )
             singletonColumns.shrink_to_fit();
       },
       [this, &mappings, full]() {
          compress_index_vector( mappings.second, emptyColumns );
          if( full )
             emptyColumns.shrink_to_fit();
       },
       [this, &mappings]() {
          for( PresolveMethod<REAL>* observer : compress_observers )
             observer->compress( mappings.first, mappings.second );
       } );

   lastcompress_ndelrows = stats.ndeletedrows;
   lastcompress_ndelcols = stats.ndeletedcols;
}

template <typename REAL>
PresolveStatus
ProblemUpdate<REAL>::checkChangedActivities()
{
   ConstraintMatrix<REAL>& consmatrix = problem.getConstraintMatrix();
   const Vec<RowFlags>& rflags = consmatrix.getRowFlags();
   const Vec<REAL>& lhs = consmatrix.getLeftHandSides();
   const Vec<REAL>& rhs = consmatrix.getRightHandSides();

   PresolveStatus status = PresolveStatus::kUnchanged;
   for( int r : changed_activities )
   {
      if( rflags[r].test( RowFlag::kRedundant ) )
         continue;

      RowStatus st = problem.getRowActivities()[r].checkStatus(
          num, rflags[r], lhs[r], rhs[r] );

      switch( st )
      {
      case RowStatus::kRedundant:
         markRowRedundant( r );
         status = PresolveStatus::kReduced;
         break;
      case RowStatus::kRedundantLhs:
         consmatrix.template modifyLeftHandSide<true>( r );
         status = PresolveStatus::kReduced;
         break;
      case RowStatus::kRedundantRhs:
         consmatrix.template modifyRightHandSide<true>( r );
         status = PresolveStatus::kReduced;
         break;
      case RowStatus::kInfeasible:
         return PresolveStatus::kInfeasible;
      case RowStatus::kUnknown:
         continue;
      }
   }

   return status;
}

template <typename REAL>
void
ProblemUpdate<REAL>::flushChangedCoeffs()
{
   // apply outstanding coefficient change
   if( !matrix_buffer.empty() )
   {
      const Vec<REAL>& lbs = problem.getLowerBounds();
      const Vec<REAL>& ubs = problem.getUpperBounds();
      const Vec<ColFlags>& cflags = problem.getColFlags();
      Vec<RowActivity<REAL>>& activities = problem.getRowActivities();

      auto coeffChanged = [this, &lbs, &cflags, &ubs, &activities](
                              int row, int col, REAL oldval, REAL newval ) {
         update_activities_after_coeffchange(
             lbs[col], ubs[col], cflags[col], oldval, newval, activities[row],
             [this, row]( ActivityChange actChange,
                          RowActivity<REAL>& activity ) {
                update_activity( actChange, row, activity );
             } );
         ++stats.ncoefchgs;
         // TODO update up/down-locks -> so that i.e. DualFix can use it
      };

      problem.getConstraintMatrix().changeCoefficients(
          matrix_buffer, singletonRows, singletonColumns, emptyColumns,
          activities, coeffChanged );

      matrix_buffer.clear();
   }
}

template <typename REAL>
PresolveStatus
ProblemUpdate<REAL>::flush()
{
   PresolveStatus status = PresolveStatus::kUnchanged;

   const Vec<REAL>& lbs = problem.getLowerBounds();
   const Vec<REAL>& ubs = problem.getUpperBounds();
   Vec<RowFlags>& rflags = problem.getRowFlags();
   const Vec<ColFlags>& cflags = problem.getColFlags();
   Vec<RowActivity<REAL>>& activities = problem.getRowActivities();
   Vec<Locks>& locks = problem.getLocks();
   ConstraintMatrix<REAL>& consMatrix = problem.getConstraintMatrix();

   // apply outstanding coefficient change
   flushChangedCoeffs();

   // remove all singleton rows after applying the coefficient changes
   if( !singletonRows.empty() )
   {
      for( int row : singletonRows )
      {
         if( removeSingletonRow( row ) == PresolveStatus::kInfeasible )
         {
            Message::debug(
                this, "[{}:{}] removeSingletonRow detected infeasible row\n",
                __FILE__, __LINE__ );
            return PresolveStatus::kInfeasible;
         }
      }

      singletonRows.clear();
   }

   // check rows with changed activities for redundancy or infeasibility
   // and remove them from the changed activities vector
   if( checkChangedActivities() == PresolveStatus::kInfeasible )
      return PresolveStatus::kInfeasible;

   auto iter =
       std::remove_if( changed_activities.begin(), changed_activities.end(),
                       [&rflags]( int row ) {
                          return rflags[row].test( RowFlag::kRedundant );
                       } );

   changed_activities.erase( iter, changed_activities.end() );

   // remove constants of fixed columns
   removeFixedCols();

   // delete fixed columns and redundant rows form the matrix
   // TODO update locks in delete rows and cols function
   consMatrix.deleteRowsAndCols( redundant_rows, deleted_cols, activities,
                                 singletonRows, singletonColumns,
                                 emptyColumns );

   // remove singleton columns from list of singleton columns if they are not
   // singletons anymore
   if( !singletonColumns.empty() )
   {
      const Vec<int>& colsizes = problem.getColSizes();
      int k = 0;
      int i;
      assert( firstNewSingletonCol <= singletonColumns.size() );
      for( i = 0; i != firstNewSingletonCol; ++i )
      {
         if( colsizes[singletonColumns[i]] != 1 )
            ++k;
         else if( k != 0 )
            singletonColumns[i - k] = singletonColumns[i];
      }

      firstNewSingletonCol -= k;

      int nsingletoncols = static_cast<int>( singletonColumns.size() );
      assert( i <= nsingletoncols );
      for( ; i != nsingletoncols; ++i )
      {
         if( colsizes[singletonColumns[i]] != 1 )
            ++k;
         else if( k != 0 )
            singletonColumns[i - k] = singletonColumns[i];
      }

      nsingletoncols -= k;
      singletonColumns.resize( nsingletoncols );

      assert( firstNewSingletonCol >= 0 &&
              firstNewSingletonCol <= nsingletoncols );
      assert( std::all_of( singletonColumns.begin(), singletonColumns.end(),
                           [&]( int col ) { return colsizes[col] == 1; } ) );
   }

   // fix empty columns
   if( removeEmptyColumns() == PresolveStatus::kUnbndOrInfeas )
      return PresolveStatus::kUnbndOrInfeas;

   return PresolveStatus::kReduced;
}

template <typename REAL>
void
ProblemUpdate<REAL>::clearStates()
{
   // clear states of rows
   for( int row : dirty_row_states )
      row_state[row] = State::kUnmodified;

   dirty_row_states.clear();

   assert(
       std::all_of( row_state.begin(), row_state.end(), []( Flags<State> s ) {
          return s.equal( State::kUnmodified );
       } ) );

   // clear states of columns
   for( int col : dirty_col_states )
      col_state[col] = State::kUnmodified;

   dirty_col_states.clear();

   assert(
       std::all_of( col_state.begin(), col_state.end(), []( Flags<State> s ) {
          return s.equal( State::kUnmodified );
       } ) );

   if( presolveOptions.compressfac != 0 )
   {
      if( ( problem.getNCols() > 100 &&
            getNActiveCols() <
                problem.getNCols() * presolveOptions.compressfac ) ||
          ( problem.getNRows() > 100 &&
            getNActiveRows() <
                problem.getNRows() * presolveOptions.compressfac ) )
         compress();
   }
}

template <typename REAL>
void
ProblemUpdate<REAL>::removeFixedCols()
{
   Objective<REAL>& obj = problem.getObjective();
   const Vec<REAL>& lbs = problem.getLowerBounds();
   const Vec<ColFlags>& cflags = problem.getColFlags();
   Vec<RowActivity<REAL>>& activities = problem.getRowActivities();
   ConstraintMatrix<REAL>& consMatrix = problem.getConstraintMatrix();
   Vec<RowFlags>& rflags = consMatrix.getRowFlags();
   Vec<REAL>& lhs = consMatrix.getLeftHandSides();
   Vec<ColFlags>& colFlags = problem.getColFlags();
   Vec<REAL>& rhs = consMatrix.getRightHandSides();

   for( int col : deleted_cols )
   {
      if( !cflags[col].test( ColFlag::kFixed ) )
         continue;

      if( cflags[col].test( ColFlag::kLbInf ) )
      {
         postsolve.notifyFixedInfCol( col, -1, problem.getUpperBounds()[col],
                                      problem );
         continue;
      }
      if( cflags[col].test( ColFlag::kUbInf ) )
      {
         postsolve.notifyFixedInfCol( col, 1, lbs[col], problem );
         continue;
      }

      assert( lbs[col] == problem.getUpperBounds()[col] );
      postsolve.notifyFixedCol( col, lbs[col] );

      // if it is fixed to zero activities and sides do not need to be
      // updated
      if( lbs[col] == 0 )
         continue;

      // update objective offset
      if( obj.coefficients[col] != 0 )
      {
         obj.offset += lbs[col] * obj.coefficients[col];
         obj.coefficients[col] = 0;
      }

      // fixed to nonzero value, so update sides and activities
      auto colvec = consMatrix.getColumnCoefficients( col );
      int collen = colvec.getLength();
      const int* colrows = colvec.getIndices();
      const REAL* colvals = colvec.getValues();

      for( int i = 0; i != collen; ++i )
      {
         int row = colrows[i];

         // if the row is redundant it will also be removed and does not need
         // to be updated
         if( rflags[row].test( RowFlag::kRedundant ) )
            continue;

         // subtract constant contribution from activity and sides
         REAL constant = lbs[col] * colvals[i];
         activities[row].min -= constant;
         activities[row].max -= constant;

         if( !rflags[row].test( RowFlag::kLhsInf ) )
            lhs[row] -= constant;

         if( !rflags[row].test( RowFlag::kRhsInf ) )
            rhs[row] -= constant;

         // due to numerics a ranged row can become an equality
         if( !rflags[row].test( RowFlag::kLhsInf, RowFlag::kRhsInf,
                                RowFlag::kEquation ) &&
             lhs[row] == rhs[row] )
            rflags[row].set( RowFlag::kEquation );
      }
   }
}

template <typename REAL>
PresolveStatus
ProblemUpdate<REAL>::trivialColumnPresolve()
{
   Vec<REAL>& lbs = problem.getLowerBounds();
   Vec<REAL>& ubs = problem.getUpperBounds();
   Vec<ColFlags>& cflags = problem.getColFlags();
   Vec<int>& colsize = problem.getColSizes();
   Vec<REAL>& obj = problem.getObjective().coefficients;
   Vec<Locks>& locks = problem.getLocks();

   PresolveStatus status = PresolveStatus::kUnchanged;

   for( int col = 0; col != problem.getNCols(); ++col )
   {
      if( cflags[col].test( ColFlag::kInactive ) )
         continue;

      // for integral columns round the bounds to integral values
      roundIntegralColumns( lbs, ubs, col, cflags, status );

      mark_huge_values( lbs, ubs, cflags, col );

      if( !cflags[col].test( ColFlag::kUnbounded ) )
      {
         if( lbs[col] > ubs[col] )
         {
            Message::debug(
                this, "[{}:{}] trivial presolve detected conflicting bounds\n",
                __FILE__, __LINE__ );
            return PresolveStatus::kInfeasible;
         }

         // remember fixed columns
         if( lbs[col] == ubs[col] )
         {
            markColFixed( col );
            status = PresolveStatus::kReduced;
            // TODO why not continue with this row
            continue;
         }
      }

      status = apply_dualfix( lbs, ubs, cflags, obj, locks, col );
      // TODO why not continue with this row
      if( status == PresolveStatus::kUnbndOrInfeas )
         return status;
      else if( status == PresolveStatus::kReduced )
         continue;

      switch( colsize[col] )
      {
      case 0:
         emptyColumns.push_back( col );
         break;
      case 1:
         singletonColumns.push_back( col );
      }
   }

   return status;
}

template <typename REAL>
PresolveStatus
ProblemUpdate<REAL>::apply_dualfix( Vec<REAL>& lbs, Vec<REAL>& ubs,
                                    Vec<ColFlags>& cflags, const Vec<REAL>& obj,
                                    const Vec<Locks>& locks, int col )
{
   if( is_dualfix_enabled( obj, col ) )
   {
      if( locks[col].down == 0 && obj[col] >= 0 )
      {
         if( cflags[col].test( ColFlag::kLbInf ) )
         {
            if( obj[col] != 0 )
            {
               Message::debug( this,
                               "[{}:{}] dual fixing in trivial presolve "
                               "detected status UNBND_OR_INFEAS\n",
                               __FILE__, __LINE__ );
               return PresolveStatus::kUnbndOrInfeas;
            }
         }
         else
         {
            ubs[col] = lbs[col];
            cflags[col].unset( ColFlag::kUbInf );
            ++stats.nboundchgs;

            markColFixed( col );
            return PresolveStatus::kReduced;
         }
      }

      if( locks[col].up == 0 && obj[col] <= 0 )
      {
         if( cflags[col].test( ColFlag::kUbInf ) )
         {
            if( obj[col] != 0 )
            {
               Message::debug( this,
                               "[{}:{}] dual fixing in trivial presolve "
                               "detected status UNBND_OR_INFEAS\n",
                               __FILE__, __LINE__ );
               return PresolveStatus::kUnbndOrInfeas;
            }
         }
         else
         {
            lbs[col] = ubs[col];
            cflags[col].unset( ColFlag::kLbInf );
            ++stats.nboundchgs;

            markColFixed( col );

            return PresolveStatus::kReduced;
         }
      }
   }
   return PresolveStatus::kUnchanged;
}

template <typename REAL>
bool
ProblemUpdate<REAL>::is_dualfix_enabled( const Vec<REAL>& obj, int col ) const
{
   bool dualfix;
   switch( presolveOptions.dualreds )
   {
   default:
      assert( false );
   case 0:
      dualfix = false;
      break;
   case 1:
      dualfix = obj[col] != 0;
      break;
   case 2:
      dualfix = true;
   }
   return dualfix;
}

template <typename REAL>
void
ProblemUpdate<REAL>::mark_huge_values( const Vec<REAL>& lbs,
                                       const Vec<REAL>& ubs,
                                       Vec<ColFlags>& cflags, int col )
{
   if( !cflags[col].test( ColFlag::kLbInf ) && num.isHugeVal( lbs[col] ) )
      cflags[col].set( ColFlag::kLbHuge );

   if( !cflags[col].test( ColFlag::kUbInf ) && num.isHugeVal( ubs[col] ) )
      cflags[col].set( ColFlag::kUbHuge );
}

// TODO: for me think about it
template <typename REAL>
void
ProblemUpdate<REAL>::roundIntegralColumns( Vec<REAL>& lbs, Vec<REAL>& ubs,
                                           int col, Vec<ColFlags>& cflags,
                                           PresolveStatus& status )
{
   if( cflags[col].test( ColFlag::kIntegral ) )
   {
      if( !cflags[col].test( ColFlag::kLbInf ) )
      {
         REAL ceillb = ceil( lbs[col] );
         if( ceillb != lbs[col] )
         {
            ++stats.nboundchgs;
            lbs[col] = ceillb;
            status = PresolveStatus::kReduced;
         }
      }

      if( !cflags[col].test( ColFlag::kUbInf ) )
      {
         REAL floorub = floor( ubs[col] );
         if( floorub != ubs[col] )
         {
            ++stats.nboundchgs;
            ubs[col] = floorub;
            status = PresolveStatus::kReduced;
         }
      }
   }
}

template <typename REAL>
PresolveStatus
ProblemUpdate<REAL>::trivialRowPresolve()
{
   ConstraintMatrix<REAL>& consMatrix = problem.getConstraintMatrix();
   Vec<int>& rowsize = consMatrix.getRowSizes();
   Vec<RowFlags>& rflags = consMatrix.getRowFlags();
   Vec<RowActivity<REAL>>& activities = problem.getRowActivities();
   const Vec<REAL>& lhs = consMatrix.getLeftHandSides();
   const Vec<REAL>& rhs = consMatrix.getRightHandSides();

   assert( activities.size() == problem.getNRows() );
   PresolveStatus status = PresolveStatus::kUnchanged;

   for( int row = 0; row != problem.getNRows(); ++row )
   {
      switch( rowsize[row] )
      {
      case 0:
         if( !rflags[row].test( RowFlag::kLhsInf ) &&
             num.isFeasGT( lhs[row], 0 ) )
         {
            Message::debug(
                this, "[{}:{}] trivial presolve detected infeasible row\n",
                __FILE__, __LINE__ );
            return PresolveStatus::kInfeasible;
         }
         if( !rflags[row].test( RowFlag::kRhsInf ) &&
             num.isFeasLT( rhs[row], 0 ) )
         {
            Message::debug(
                this, "[{}:{}] trivial presolve detected infeasible row\n",
                __FILE__, __LINE__ );
            return PresolveStatus::kInfeasible;
         }
         rflags[row].set( RowFlag::kRedundant );
         rowsize[row] = -1;
         status = PresolveStatus::kReduced;
         break;
      case 1:
         status = removeSingletonRow( row );
         if( status == PresolveStatus::kInfeasible )
         {
            Message::debug(
                this, "[{}:{}] removeSingletonRow detected infeasible row\n",
                __FILE__, __LINE__ );
            return status;
         }
         break;
      default:
      {
         RowStatus st = activities[row].checkStatus( num, rflags[row], lhs[row],
                                                     rhs[row] );
         switch( st )
         {
         case RowStatus::kRedundant:
            markRowRedundant( row );
            status = PresolveStatus::kReduced;
            break;
         case RowStatus::kRedundantLhs:
            consMatrix.template modifyLeftHandSide<true>( row );
            status = PresolveStatus::kReduced;
            cleanupSmallCoefficients( row );
            break;
         case RowStatus::kRedundantRhs:
            consMatrix.template modifyRightHandSide<true>( row );
            status = PresolveStatus::kReduced;
            cleanupSmallCoefficients( row );
            break;
         case RowStatus::kInfeasible:
            return PresolveStatus::kInfeasible;
         case RowStatus::kUnknown:
            if( !rflags[row].test( RowFlag::kRhsInf, RowFlag::kLhsInf,
                                   RowFlag::kEquation ) )
            {
               assert( !rflags[row].test( RowFlag::kRhsInf ) );
               assert( !rflags[row].test( RowFlag::kLhsInf ) );
               assert( !rflags[row].test( RowFlag::kEquation ) );
               if( lhs[row] == rhs[row] )
                  rflags[row].set( RowFlag::kEquation );
            }
            cleanupSmallCoefficients( row );
         }
      }
      }

      // row should be either redundant, or the equality flag must be set
      // correctly
      assert( rflags[row].test( RowFlag::kRedundant ) ||
              ( !rflags[row].test( RowFlag::kEquation ) &&
                ( rflags[row].test( RowFlag::kLhsInf, RowFlag::kRhsInf ) ||
                  lhs[row] != rhs[row] ) ) ||
              ( rflags[row].test( RowFlag::kEquation ) &&
                lhs[row] == rhs[row] &&
                !rflags[row].test( RowFlag::kLhsInf, RowFlag::kRhsInf ) ) );
   }

   flushChangedCoeffs();

   return status;
}

template <typename REAL>
PresolveStatus
ProblemUpdate<REAL>::trivialPresolve()
{
   if( presolveOptions.dualreds != 0 )
      problem.recomputeLocks();

   PresolveStatus status = trivialColumnPresolve();
   if( status == PresolveStatus::kInfeasible ||
       status == PresolveStatus::kUnbndOrInfeas )
      return status;

   problem.recomputeAllActivities();
   status = trivialRowPresolve();
   if( status == PresolveStatus::kInfeasible ||
       status == PresolveStatus::kUnbndOrInfeas )
      return status;

   removeFixedCols();

   problem.getConstraintMatrix().deleteRowsAndCols(
       redundant_rows, deleted_cols, problem.getRowActivities(), singletonRows,
       singletonColumns, emptyColumns );

   for( int row : singletonRows )
   {
      status = removeSingletonRow( row );
      if( status == PresolveStatus::kInfeasible )
      {
         Message::debug( this,
                         "[{}:{}] removeSingletonRow detected infeasible row\n",
                         __FILE__, __LINE__ );
         return status;
      }
   }

   if( !singletonColumns.empty() )
   {
      int numNewSingletonCols =
          static_cast<int>( singletonColumns.size() ) - firstNewSingletonCol;
      assert( numNewSingletonCols >= 0 );
      auto it = std::remove_if(
          singletonColumns.begin(), singletonColumns.end(),
          [this]( int c ) { return problem.getColSizes()[c] != 1; } );
      singletonColumns.erase( it, singletonColumns.end() );
      firstNewSingletonCol =
          std::max( 0, static_cast<int>( singletonColumns.size() ) -
                           numNewSingletonCols );
   }

   status = checkChangedActivities();
   if( status == PresolveStatus::kInfeasible ||
       status == PresolveStatus::kUnbndOrInfeas )
      return status;

   changed_activities.clear();

   const Vec<RowFlags>& rflags = problem.getRowFlags();

   for( int r = 0; r != problem.getNRows(); ++r )
   {
      if( rflags[r].test( RowFlag::kRedundant ) )
         continue;

      RowActivity<REAL>& activity = problem.getRowActivities()[r];
      if( activity.ninfmin == 0 || activity.ninfmax == 0 ||
          ( activity.ninfmax == 1 && !rflags[r].test( RowFlag::kLhsInf ) ) ||
          ( activity.ninfmin == 1 && !rflags[r].test( RowFlag::kRhsInf ) ) )
         changed_activities.push_back( r );
   }

   flush();

   return status;
}


template <typename REAL>
PresolveStatus
ProblemUpdate<REAL>::removeSingletonRow( int row )
{
   ConstraintMatrix<REAL>& consMatrix = problem.getConstraintMatrix();
   const Vec<int>& rowsize = consMatrix.getRowSizes();
   Vec<RowFlags>& rflags = consMatrix.getRowFlags();

   PresolveStatus status = PresolveStatus::kUnchanged;

   if( rowsize[row] != 1 || rflags[row].test( RowFlag::kRedundant ) )
      return status;

   auto rowvec = consMatrix.getRowCoefficients( row );

   assert( rowvec.getLength() == 1 );

   const REAL val = rowvec.getValues()[0];
   const int col = rowvec.getIndices()[0];
   const REAL lhs = consMatrix.getLeftHandSides()[row];
   const REAL rhs = consMatrix.getRightHandSides()[row];

   // TODO: save row for dual-postsolve.

   if( rflags[row].test( RowFlag::kEquation ) )
      status = fixCol( col, rhs / val );
   else if( val < 0 )
   {
      if( !rflags[row].test( RowFlag::kLhsInf ) )
         status = changeUB( col, lhs / val );

      if( !rflags[row].test( RowFlag::kRhsInf ) &&
          status != PresolveStatus::kInfeasible )
         status = changeLB( col, rhs / val );
   }
   else
   {
      assert( val > 0 );

      if( !rflags[row].test( RowFlag::kLhsInf ) )
         status = changeLB( col, lhs / val );

      if( !rflags[row].test( RowFlag::kRhsInf ) &&
          status != PresolveStatus::kInfeasible )
         status = changeUB( col, rhs / val );
   }

   markRowRedundant( row );

   return status;
}

template <typename REAL>
void
ProblemUpdate<REAL>::cleanupSmallCoefficients( int row )
{
   ConstraintMatrix<REAL>& consMatrix = problem.getConstraintMatrix();
   const Vec<REAL>& lbs = problem.getLowerBounds();
   const Vec<REAL>& ubs = problem.getUpperBounds();
   const Vec<ColFlags>& cflags = problem.getColFlags();

   auto rowvec = consMatrix.getRowCoefficients( row );

   // arrays with nonzeros and their column index of this row
   const REAL* values = rowvec.getValues();
   const int* columns = rowvec.getIndices();

   // number of nonzeros in row, i.e. length of arrays above
   int len = rowvec.getLength();

   // acces to sides of the given row
   REAL& lhs = consMatrix.getLeftHandSides()[row];
   REAL& rhs = consMatrix.getRightHandSides()[row];
   RowFlags& rowf = consMatrix.getRowFlags()[row];

   // loop over non-zeros of this row
   REAL total_mod = 0;
   for( int i = 0; i != len; ++i )
   {
      int col = columns[i];

      if( cflags[col].test( ColFlag::kUnbounded, ColFlag::kInactive ) )
         continue;

      assert( ubs[col] > lbs[col] );

      // model Cleanup
      REAL absval = abs( values[i] );

      if( absval < presolveOptions.minabscoeff )
      {
         matrix_buffer.addEntry( row, col, 0 );

         Message::debug( this, "removed tiny coefficient with value {}\n",
                         double( values[i] ) );

         continue;
      }

      if( absval <= 1e-3 &&
          absval * ( ubs[col] - lbs[col] ) * len <= 1e-2 * num.getFeasTol() )
      {
         REAL temp_total_mod = total_mod + absval * ( ubs[col] - lbs[col] );
         if( temp_total_mod <= 0.1 * num.getFeasTol() )
         {
            matrix_buffer.addEntry( row, col, 0 );

            Message::debug( this, "removed small coefficient with value {}\n",
                            double( values[i] ) );

            if( lbs[col] != 0 )
            {
               REAL sidechange = values[i] * lbs[col];
               if( !rowf.test( RowFlag::kRhsInf ) )
               {
                  rhs -= sidechange;
                  ++stats.nsidechgs;
               }
               if( !rowf.test( RowFlag::kLhsInf ) )
               {
                  lhs -= sidechange;
                  ++stats.nsidechgs;
               }

               // due to numerics a ranged row can become an equality
               if( !rowf.test( RowFlag::kLhsInf, RowFlag::kRhsInf,
                               RowFlag::kEquation ) &&
                   lhs == rhs )
                  rowf.set( RowFlag::kEquation );
            }

            total_mod = temp_total_mod;
         }
      }
   }
}

template <typename REAL>
PresolveStatus
ProblemUpdate<REAL>::removeEmptyColumns()
{
   if( presolveOptions.dualreds != 0 && !emptyColumns.empty() )
   {
      Objective<REAL>& obj = problem.getObjective();
      VariableDomains<REAL>& domains = problem.getVariableDomains();
      Vec<int>& colsize = problem.getConstraintMatrix().getColSizes();
      for( int col : emptyColumns )
      {
         if( colsize[col] != 0 )
            continue;

         if( presolveOptions.dualreds == 1 && obj.coefficients[col] == 0 )
            continue;

         if( !domains.flags[col].test( ColFlag::kInactive ) )
         {
            assert( colsize[col] == 0 );

            REAL fixval;

            if( obj.coefficients[col] == 0 )
            {
               fixval = 0;

               if( !domains.flags[col].test( ColFlag::kUbInf ) &&
                   domains.upper_bounds[col] < 0 )
                  fixval = domains.upper_bounds[col];
               else if( !domains.flags[col].test( ColFlag::kLbInf ) &&
                        domains.lower_bounds[col] > 0 )
                  fixval = domains.lower_bounds[col];
            }
            else
            {
               if( obj.coefficients[col] < 0 )
               {
                  if( domains.flags[col].test( ColFlag::kUbInf ) )
                     return PresolveStatus::kUnbndOrInfeas;

                  fixval = domains.upper_bounds[col];
               }
               else
               {
                  assert( obj.coefficients[col] > 0 );
                  if( domains.flags[col].test( ColFlag::kLbInf ) )
                     return PresolveStatus::kUnbndOrInfeas;

                  fixval = domains.lower_bounds[col];
               }

               obj.offset += obj.coefficients[col] * fixval;
               obj.coefficients[col] = 0;
            }

            postsolve.notifyFixedCol( col, fixval );
            domains.flags[col].set( ColFlag::kFixed );

            ++stats.ndeletedcols;

            if( domains.flags[col].test( ColFlag::kIntegral ) )
               --problem.getNumIntegralCols();
            else
               --problem.getNumContinuousCols();
         }

         assert( obj.coefficients[col] == 0 );

         colsize[col] = -1;
      }

      emptyColumns.clear();

      return PresolveStatus::kReduced;
   }

   return PresolveStatus::kUnchanged;
}

/// returns true if the given transaction conflicts with the current state of
/// changes and false otherwise
template <typename REAL>
ConflictType
ProblemUpdate<REAL>::checkTransactionConflicts( const Reduction<REAL>* first,
                                                const Reduction<REAL>* last )
{
   // check if transaction conflicts with current state
   for( const Reduction<REAL>* iter = first; iter != last; ++iter )
   {
      const Reduction<REAL>& reduction = *iter;

      if( reduction.row >= 0 && reduction.col >= 0 )
      {
         // modification of a coefficient is only allowed if the coefficients
         // is neither locked
         if( col_state[reduction.col].test( State::kLocked ) ||
             row_state[reduction.row].test( State::kLocked ) )
            return ConflictType::kConflict;
      }
      else if( reduction.row < 0 )
      {
         assert( reduction.col >= 0 );
         int colop = reduction.row;
         switch( colop )
         {
         case ColReduction::LOCKED_STRONG:
         case ColReduction::LOCKED:
            // if the transaction wants to lock the column it must not be
            // modifed yet
            if( col_state[reduction.col].test( State::kModified ) )
               return ConflictType::kConflict;
            break;
         case ColReduction::OBJECTIVE:
            // if the transaction wants to modify the column by changing its
            // objective coefficient the column must not be locked
            if( col_state[reduction.col].test( State::kLocked ) )
               return ConflictType::kConflict;
            break;
         case ColReduction::BOUNDS_LOCKED:
            if( col_state[reduction.col].test( State::kBoundsModified ) )
               return ConflictType::kConflict;
            break;
         case ColReduction::PARALLEL:
         case ColReduction::SUBSTITUTE_OBJ:
            // TODO I think columns in the substitution equation need to be
            // checked for locks since they will be modified?
            break;
         case ColReduction::SUBSTITUTE:
         case ColReduction::REPLACE:
            // we postponed the substitution to be performed last
            if( postponeSubstitutions )
               return ConflictType::kPostpone;

            // TODO I think columns in the substitution equation need to be
            // checked for locks since they will be modified? (e.g. strong
            // locks from dual fixing)
            break;
         default:
            break;
         }
      }
      else
      {
         assert( reduction.row >= 0 && reduction.col < 0 );
         int rowop = reduction.col;
         switch( rowop )
         {
         case RowReduction::LOCKED_STRONG:
         case RowReduction::LOCKED:
            // if the transaction wants to lock the row it must not be
            // modified yet
            if( row_state[reduction.row].test( State::kModified,
                                               State::kBoundsModified ) )
               return ConflictType::kConflict;
            break;
         case RowReduction::LHS_INF:
         case RowReduction::LHS:
            // if the transaction wants to modify a rows left hand side the
            // corresponding row must not be locked
            if( row_state[reduction.row].test( State::kLocked ) )
               return ConflictType::kConflict;
            break;
         case RowReduction::RHS_INF:
         case RowReduction::RHS:
            // if the transaction wants to modify a rows right hand side the
            // corresponding row must neither be locked nor modified yet
            if( row_state[reduction.row].test( State::kLocked ) )
               return ConflictType::kConflict;
            break;
         case RowReduction::SPARSIFY:
            if( postponeSubstitutions )
               return ConflictType::kPostpone;
         }
      }
   }

   // no conflicts found
   return ConflictType::kNoConflict;
}

/// returns true if the given transaction was applied and false otherwise
template <typename REAL>
ApplyResult
ProblemUpdate<REAL>::applyTransaction( const Reduction<REAL>* first,
                                       const Reduction<REAL>* last )
{
   Objective<REAL>& objective = problem.getObjective();
   Vec<REAL>& lbs = problem.getLowerBounds();
   Vec<REAL>& ubs = problem.getUpperBounds();
   Vec<ColFlags>& cflags = problem.getColFlags();
   ConstraintMatrix<REAL>& constraintMatrix = problem.getConstraintMatrix();
   Vec<RowFlags>& rflags = constraintMatrix.getRowFlags();

   auto updateActivity = [this]( ActivityChange actChange, int rowid,
                                 RowActivity<REAL>& activity ) {
      update_activity( actChange, rowid, activity );
   };

   // check if transaction conflicts with current state
   ConflictType conflictType = checkTransactionConflicts( first, last );

   if( conflictType == ConflictType::kConflict )
      return ApplyResult::kRejected;
   else if( conflictType == ConflictType::kPostpone )
      return ApplyResult::kPostponed;

   for( auto iter = first; iter != last; ++iter )
   {
      const auto& reduction = *iter;

      if( reduction.row >= 0 && reduction.col >= 0 )
      {
         setRowState( reduction.row, State::kModified );
         setColState( reduction.col, State::kModified );

         matrix_buffer.addEntry( reduction.row, reduction.col,
                                 reduction.newval );
      }
      else if( reduction.row < 0 )
      {
         assert( reduction.col >= 0 );
         int colop = reduction.row;
         switch( colop )
         {
         case ColReduction::NONE:
            assert( false );
            break;
         case ColReduction::LOCKED_STRONG:
            setColState( reduction.col, State::kLocked );
            break;
         case ColReduction::OBJECTIVE:
            setColState( reduction.col, State::kModified );
            objective.coefficients[reduction.col] = reduction.newval;
            break;
         case ColReduction::FIXED:
         {
            if( fixCol( reduction.col, reduction.newval ) ==
                PresolveStatus::kInfeasible )
               return ApplyResult::kInfeasible;
            break;
         }
         case ColReduction::FIXED_INFINITY:
         {
            if( fixColInfinity( reduction.col, reduction.newval ) ==
                PresolveStatus::kInfeasible )
               return ApplyResult::kInfeasible;
            break;
         }
         case ColReduction::LOWER_BOUND:
         {
            if( changeLB( reduction.col, reduction.newval ) ==
                PresolveStatus::kInfeasible )
               return ApplyResult::kInfeasible;
            break;
         }
         case ColReduction::UPPER_BOUND:
         {
            if( changeUB( reduction.col, reduction.newval ) ==
                PresolveStatus::kInfeasible )
               return ApplyResult::kInfeasible;
            break;
         }
         case ColReduction::IMPL_INT:
         {
            if( !cflags[reduction.col].test( ColFlag::kInactive ) )
            {
               cflags[reduction.col].set( ColFlag::kImplInt );
               if( !cflags[reduction.col].test( ColFlag::kLbInf ) )
               {
                  if( changeLB( reduction.col, lbs[reduction.col] ) ==
                      PresolveStatus::kInfeasible )
                     return ApplyResult::kInfeasible;
               }

               if( !cflags[reduction.col].test( ColFlag::kUbInf ) )
               {
                  if( changeUB( reduction.col, ubs[reduction.col] ) ==
                      PresolveStatus::kInfeasible )
                     return ApplyResult::kInfeasible;
               }
            }
            break;
         }
         case ColReduction::SUBSTITUTE:
         {
            int col = reduction.col;
            int equalityrow = static_cast<int>( reduction.newval );

            if( constraintMatrix.getRowCoefficients( equalityrow )
                    .getLength() == 1 )
            {
               assert( !rflags[equalityrow].test( RowFlag::kLhsInf,
                                                  RowFlag::kRhsInf ) );
               REAL val = constraintMatrix.getLeftHandSides()[equalityrow] /
                          constraintMatrix.getRowCoefficients( equalityrow )
                              .getValues()[0];
               if( fixCol( col, val ) == PresolveStatus::kInfeasible )
                  return ApplyResult::kInfeasible;
               break;
            }

            assert( row_state[equalityrow].equal( State::kUnmodified ) );
            assert( !col_state[col].test( State::kBoundsModified ) );

            // check that the conditions for substitution are verified
            if( !constraintMatrix.checkAggregationSparsityCondition(
                    col, constraintMatrix.getRowCoefficients( equalityrow ),
                    presolveOptions.maxfillinpersubstitution,
                    presolveOptions.maxshiftperrow, intbuffer ) )
               return ApplyResult::kRejected;

            const auto colvec = constraintMatrix.getColumnCoefficients( col );
            const int* colindices = colvec.getIndices();
            const int nbrelevantrows = colvec.getLength();

            assert(
                !cflags[col].test( ColFlag::kSubstituted, ColFlag::kFixed ) );
            cflags[col].set( ColFlag::kSubstituted );

            // change the objective coefficients and offset
            problem.substituteVarInObj( num, col, equalityrow );

            // update row states
            for( int k = 0; k < nbrelevantrows; ++k )
               setRowState( colindices[k], State::kModified );

            // update col states
            const auto rowvec =
                constraintMatrix.getRowCoefficients( equalityrow );
            const int length = rowvec.getLength();
            const int* indices = rowvec.getIndices();

            for( int j = 0; j < length; ++j )
               setColState( indices[j], State::kModified );

            auto eqRHS = constraintMatrix.getLeftHandSides()[equalityrow];

            postsolve.notifySubstitution( col, rowvec, eqRHS );
            // make the changes in the constraint matrix
            constraintMatrix.aggregate(
                num, col, rowvec, eqRHS, problem.getVariableDomains(),
                intbuffer, realbuffer, tripletbuffer, changed_activities,
                problem.getRowActivities(), singletonRows, singletonColumns,
                emptyColumns, stats.nrounds );

            stats.ncoefchgs += length * nbrelevantrows;

            assert( constraintMatrix.getRowSizes()[equalityrow] == -1 );
            assert( constraintMatrix.getRowCoefficients( equalityrow )
                        .getLength() == 0 );
            assert( constraintMatrix.getLeftHandSides()[equalityrow] ==
                    REAL{ 0 } );
            assert( constraintMatrix.getRightHandSides()[equalityrow] ==
                    REAL{ 0 } );
            assert( constraintMatrix.getColSizes()[col] == -1 );
            assert( constraintMatrix.getColumnCoefficients( col ).getLength() ==
                    REAL{ 0 } );
            assert( objective.coefficients[col] == REAL{ 0 } );

            assert( row_state[equalityrow].test( State::kModified ) );
            assert( col_state[col].test( State::kModified ) );

            // statistics
            ++stats.ndeletedcols;

            // statistics
            ++stats.ndeletedrows;

            if( cflags[col].test( ColFlag::kIntegral ) )
               --problem.getNumIntegralCols();
            else
               --problem.getNumContinuousCols();

            if( constraintMatrix.getLeftHandSides()[equalityrow] != 0 )
               stats.nsidechgs += 2 * nbrelevantrows;
            break;
         }
         case ColReduction::SUBSTITUTE_OBJ:
         {
            int col = reduction.col;
            int equalityrow = static_cast<int>( reduction.newval );

            assert( !cflags[col].test( ColFlag::kInactive ) );
            cflags[col].set( ColFlag::kSubstituted );

            // change the objective coefficients and offset
            problem.substituteVarInObj( num, col, equalityrow );

            auto colvec = constraintMatrix.getColumnCoefficients( col );

            if( cflags[col].test( ColFlag::kLbUseless ) || lbs[col] != 0 )
               update_activities_after_boundchange(
                   colvec.getValues(), colvec.getIndices(), colvec.getLength(),
                   BoundChange::kLower, lbs[col], REAL{ 0 },
                   cflags[col].test( ColFlag::kLbUseless ),
                   problem.getRowActivities(), updateActivity );

            if( cflags[col].test( ColFlag::kUbUseless ) || ubs[col] != 0 )
               update_activities_after_boundchange(
                   colvec.getValues(), colvec.getIndices(), colvec.getLength(),
                   BoundChange::kUpper, ubs[col], REAL{ 0 },
                   cflags[col].test( ColFlag::kUbUseless ),
                   problem.getRowActivities(), updateActivity );

            cflags[col].unset( ColFlag::kLbUseless, ColFlag::kUbUseless );
            lbs[col] = 0;
            ubs[col] = 0;
            deleted_cols.push_back( col );

            // update col states
            const auto rowvec =
                constraintMatrix.getRowCoefficients( equalityrow );

            postsolve.notifySubstitution(
                col, rowvec, constraintMatrix.getLeftHandSides()[equalityrow] );

            const int length = rowvec.getLength();
            const int* indices = rowvec.getIndices();

            for( int j = 0; j != length; ++j )
               setColState( indices[j], State::kModified );

            // statistics
            ++stats.ndeletedcols;

            if( cflags[col].test( ColFlag::kIntegral ) )
               --problem.getNumIntegralCols();
            else
               --problem.getNumContinuousCols();

            break;
         }
         case ColReduction::PARALLEL:
         {
            int col1 = reduction.col;
            int col2 = static_cast<int>( reduction.newval );

            if( cflags[col1].test( ColFlag::kInactive ) ||
                cflags[col2].test( ColFlag::kInactive ) )
               return ApplyResult::kRejected;

            setColState( col1, State::kBoundsModified );
            setColState( col2, State::kBoundsModified );

            auto col1vec = constraintMatrix.getColumnCoefficients( col1 );
            auto col2vec = constraintMatrix.getColumnCoefficients( col2 );

            const int* inds = col1vec.getIndices();
            const REAL* vals1 = col1vec.getValues();
            const REAL* vals2 = col2vec.getValues();
            const int collen = col1vec.getLength();

            assert( collen > 0 );
            REAL col2scale = vals1[0] / vals2[0];
            assert( col2vec.getLength() == collen );

            assert( num.isEq( objective.coefficients[col1],
                              objective.coefficients[col2] * col2scale ) );

            bool col1lbinf = cflags[col1].test( ColFlag::kLbInf );
            bool col1ubinf = cflags[col1].test( ColFlag::kUbInf );
            bool col1int = cflags[col1].test( ColFlag::kIntegral );
            bool col2lbinf = cflags[col2].test( ColFlag::kLbInf );
            bool col2ubinf = cflags[col2].test( ColFlag::kUbInf );
            bool col2int = cflags[col2].test( ColFlag::kIntegral );

            postsolve.notifyParallelCols( col1, col1int, col1lbinf, lbs[col1],
                                          col1ubinf, ubs[col1], col2, col2int,
                                          col2lbinf, lbs[col2], col2ubinf,
                                          ubs[col2], col2scale );
            ++stats.ndeletedcols;

            // compute the new domains for column 2
            REAL newlb = 0;
            REAL newub = 0;
            bool newlbinf = true;
            bool newubinf = true;
            ColFlags newflags;

            newflags.set( ColFlag::kLbInf, ColFlag::kUbInf );

            // in the case that column 1 is not integral the new column
            // is also not integral regardless of whether column 2 is integral
            // or not (the necessary conditions must have been checked by the
            // presolver)
            if( cflags[col1].test( ColFlag::kIntegral ) )
            {
               --problem.getNumIntegralCols();
               newflags.set( ColFlag::kIntegral );
            }
            else if( cflags[col2].test( ColFlag::kIntegral ) )
               --problem.getNumIntegralCols();
            else
               --problem.getNumContinuousCols();

            if( col2scale < 0 )
            {
               if( !col2lbinf && !col1ubinf )
               {
                  newlb = lbs[col2] + col2scale * ubs[col1];
                  newflags.unset( ColFlag::kLbInf );
                  if( cflags[col1].test( ColFlag::kUbHuge ) ||
                      cflags[col2].test( ColFlag::kLbHuge ) )
                     newflags.set( ColFlag::kLbHuge );
               }

               if( !col2ubinf && !col1lbinf )
               {
                  newub = ubs[col2] + col2scale * lbs[col1];
                  newflags.unset( ColFlag::kUbInf );
                  if( cflags[col1].test( ColFlag::kLbHuge ) ||
                      cflags[col2].test( ColFlag::kUbHuge ) )
                     newflags.set( ColFlag::kUbHuge );
               }
            }
            else
            {
               if( !col2lbinf && !col1lbinf )
               {
                  newlb = lbs[col2] + col2scale * lbs[col1];
                  newflags.unset( ColFlag::kLbInf );
                  if( cflags[col1].test( ColFlag::kLbHuge ) ||
                      cflags[col2].test( ColFlag::kLbHuge ) )
                     newflags.set( ColFlag::kLbHuge );
               }

               if( !col2ubinf && !col1ubinf )
               {
                  newub = ubs[col2] + col2scale * ubs[col1];
                  newflags.unset( ColFlag::kUbInf );
                  if( cflags[col1].test( ColFlag::kUbHuge ) ||
                      cflags[col2].test( ColFlag::kUbHuge ) )
                     newflags.set( ColFlag::kUbHuge );
               }
            }

            // update the activities if required
            if( newflags.test( ColFlag::kLbUseless ) )
            {
               // the new columns lower bound does not contribute to the
               // activities

               if( !cflags[col2].test( ColFlag::kLbUseless ) )
               {
                  // The current bound of column 2 contributes to the activity,
                  // therefore column 1 must have a infinite or huge bound from
                  // which we keep the infinite contribution for the new columns
                  // domains. The finite constribution of the lower bound of
                  // column 2 is removed.
                  if( lbs[col2] != 0 )
                  {
                     update_activities_after_boundchange(
                         vals2, inds, collen, BoundChange::kLower, lbs[col2],
                         REAL{ 0 }, false, problem.getRowActivities(),
                         []( ActivityChange, int, const RowActivity<REAL>& ) {
                         } );
                  }
               }
               else if( col2scale < 0 )
               {
                  // The lower bound of column 2 also does not contribute to the
                  // activities, and we keep that infinite contribution for the
                  // new column. Depending on the scale we remove any finite or
                  // infinite contribution of column 1's bound. In this if case
                  // the scale is negative, so the upper bound is removed.
                  if( cflags[col1].test( ColFlag::kUbUseless ) ||
                      ubs[col1] != 0 )
                  {
                     update_activities_after_boundchange(
                         vals1, inds, collen, BoundChange::kUpper, ubs[col1],
                         REAL{ 0 }, cflags[col1].test( ColFlag::kUbUseless ),
                         problem.getRowActivities(),
                         []( ActivityChange, int, const RowActivity<REAL>& ) {
                         } );
                  }
               }
               else
               {
                  // The lower bound of column 2 also does not contribute to the
                  // activities, and we keep that infinite contribution for the
                  // new column. Depending on the scale we remove any finite or
                  // infinite contribution of column 1's bound. In this if case
                  // the scale is positive, so the lower bound is removed.
                  if( cflags[col1].test( ColFlag::kLbUseless ) ||
                      lbs[col1] != 0 )
                  {
                     update_activities_after_boundchange(
                         vals1, inds, collen, BoundChange::kLower, lbs[col1],
                         REAL{ 0 }, cflags[col1].test( ColFlag::kLbUseless ),
                         problem.getRowActivities(),
                         []( ActivityChange, int, const RowActivity<REAL>& ) {
                         } );
                  }
               }
            }

            if( newflags.test( ColFlag::kUbUseless ) )
            {
               // symmetric cases as above for the lower bound
               if( !cflags[col2].test( ColFlag::kUbUseless ) )
               {
                  if( ubs[col2] != 0 )
                  {
                     update_activities_after_boundchange(
                         vals2, inds, collen, BoundChange::kUpper, ubs[col2],
                         REAL{ 0 }, false, problem.getRowActivities(),
                         updateActivity );
                  }
               }
               else if( col2scale < 0 )
               {
                  if( cflags[col1].test( ColFlag::kLbUseless ) ||
                      lbs[col1] != 0 )
                  {
                     update_activities_after_boundchange(
                         vals1, inds, collen, BoundChange::kLower, lbs[col1],
                         REAL{ 0 }, cflags[col1].test( ColFlag::kLbUseless ),
                         problem.getRowActivities(),
                         []( ActivityChange, int, const RowActivity<REAL>& ) {
                         } );
                  }
               }
               else
               {
                  if( cflags[col1].test( ColFlag::kUbUseless ) ||
                      ubs[col1] != 0 )
                  {
                     update_activities_after_boundchange(
                         vals1, inds, collen, BoundChange::kUpper, ubs[col1],
                         REAL{ 0 }, cflags[col1].test( ColFlag::kUbUseless ),
                         problem.getRowActivities(),
                         []( ActivityChange, int, const RowActivity<REAL>& ) {
                         } );
                  }
               }
            }

            // column 1 can now be treated as if it fixed to zero
            // the flag however is not set to ColFlag::kFixed since
            // this indicates that their will be a notification to postsolve
            // about that case, instead it is set to substituted
            lbs[col1] = 0;
            ubs[col1] = 0;
            cflags[col1].unset( ColFlag::kLbUseless, ColFlag::kUbUseless );
            cflags[col1].set( ColFlag::kSubstituted );
            deleted_cols.push_back( col1 );

            // the domains of column 2 are now set column 2 bounds are set to
            // new bound values
            lbs[col2] = newlb;
            ubs[col2] = newub;
            cflags[col2] = newflags;
            break;
         }
         case ColReduction::REPLACE:
         {
            int col1 = reduction.col;
            REAL factor = reduction.newval;

            // get the rest of the information from the next reduction
            ++iter;
            assert( iter->row == ColReduction::NONE );
            int col2 = iter->col;
            REAL offset = iter->newval;

            // one variable is fixed, try to fix the other one
            if( cflags[col1].test( ColFlag::kFixed ) ||
                cflags[col2].test( ColFlag::kFixed ) )
            {
               if( !cflags[col1].test( ColFlag::kFixed,
                                       ColFlag::kSubstituted ) )
               {
                  assert( cflags[col2].test( ColFlag::kFixed ) );
                  if( fixCol( col1, factor * lbs[col2] + offset ) ==
                      PresolveStatus::kInfeasible )
                     return ApplyResult::kInfeasible;
               }
               else if( !cflags[col2].test( ColFlag::kFixed,
                                            ColFlag::kSubstituted ) )
               {
                  assert( cflags[col1].test( ColFlag::kFixed ) );
                  if( fixCol( col2, ( lbs[col1] - offset ) / factor ) ==
                      PresolveStatus::kInfeasible )
                     return ApplyResult::kInfeasible;
               }
               break;
            }

            // one variable might have been substituted
            if( cflags[col1].test( ColFlag::kFixed, ColFlag::kSubstituted ) ||
                cflags[col2].test( ColFlag::kFixed, ColFlag::kSubstituted ) )
               break;

            assert( constraintMatrix.getColSizes()[col1] > 0 &&
                    constraintMatrix.getColSizes()[col2] > 0 );

            REAL col2_imp_lb;
            REAL col2_imp_ub;
            if( factor > 0.0 )
            {
               col2_imp_lb = ( lbs[col1] - offset ) / factor;
               col2_imp_ub = ( ubs[col1] - offset ) / factor;
            }
            else
            {
               col2_imp_lb = ( ubs[col1] - offset ) / factor;
               col2_imp_ub = ( lbs[col1] - offset ) / factor;
            }
            if( col2_imp_lb > lbs[col2] )
            {
               if( changeLB( col2, col2_imp_lb ) ==
                   PresolveStatus::kInfeasible )
                  return ApplyResult::kInfeasible;
            }
            else if( col2_imp_ub < ubs[col2] )
            {
               if( changeUB( col2, col2_imp_ub ) ==
                   PresolveStatus::kInfeasible )
                  return ApplyResult::kInfeasible;
            }

            // set up the equality
            // x_1 - factor * x_2 = offset
            int indices[] = { col1, col2 };
            REAL coefficients[] = { 1.0, -factor };
            // argument needs to be sorted
            if( col1 > col2 )
            {
               std::swap( indices[0], indices[1] );
               std::swap( coefficients[0], coefficients[1] );
            }
            SparseVectorView<REAL> equalityLHS( coefficients, indices, 2 );

            // check sparsity
            if( constraintMatrix.checkAggregationSparsityCondition(
                    col1, equalityLHS, presolveOptions.maxfillinpersubstitution,
                    presolveOptions.maxshiftperrow, intbuffer ) )
            {
               auto colvec = constraintMatrix.getColumnCoefficients( col1 );
               const int* colindices = colvec.getIndices();
               int length = colvec.getLength();

               cflags[col1].set( ColFlag::kSubstituted );

               if( cflags[col1].test( ColFlag::kIntegral ) )
                  --problem.getNumIntegralCols();
               else
                  --problem.getNumContinuousCols();

               // update row flags
               for( int k = 0; k < length; ++k )
                  setRowState( colindices[k], State::kModified );

               // perform changes in matrix and sides
               postsolve.notifySubstitution( col1, equalityLHS, offset );

               constraintMatrix.aggregate(
                   num, col1, equalityLHS, offset, problem.getVariableDomains(),
                   intbuffer, realbuffer, tripletbuffer, changed_activities,
                   problem.getRowActivities(), singletonRows, singletonColumns,
                   emptyColumns, stats.nrounds );

               // update col flags
               setColState( col1, State::kModified );
               setColState( col2, State::kModified );

               // change the objective
               auto& obj = problem.getObjective();
               auto& obj_coef = obj.coefficients;
               if( obj_coef[col1] != REAL{ 0 } )
               {
                  obj_coef[col2] += obj_coef[col1] * factor;
                  if( num.isZero( obj_coef[col2] ) )
                     obj_coef[col2] = REAL{ 0 };
                  obj.offset += obj_coef[col1] * offset;
                  obj_coef[col1] = REAL{ 0 };
               }

               // statistics
               if( offset != REAL{ 0 } )
                  stats.nsidechgs += 2 * length;
               stats.ncoefchgs += 2 * length;
               ++stats.ndeletedcols;
            }
            break;
         }
         default:
            break;
         }
      }
      else
      {
         assert( reduction.row >= 0 && reduction.col < 0 );
         int rowop = reduction.col;
         switch( rowop )
         {
         case RowReduction::NONE:
            assert( false );
            break;
         case RowReduction::LOCKED_STRONG:
            setRowState( reduction.row, State::kLocked );
            break;
         case RowReduction::LHS:
            assert( rflags[reduction.row].test( RowFlag::kLhsInf ) ||
                    reduction.newval !=
                        constraintMatrix.getLeftHandSides()[reduction.row] );
            setRowState( reduction.row, State::kBoundsModified );

            if( rflags[reduction.row].test( RowFlag::kLhsInf ) )
            {
               auto rowvec =
                   constraintMatrix.getRowCoefficients( reduction.row );
               const int rowlen = rowvec.getLength();
               const int* rowcols = rowvec.getIndices();

               for( int i = 0; i != rowlen; ++i )
                  setColState( rowcols[i], State::kModified );
            }

            constraintMatrix.modifyLeftHandSide( reduction.row,
                                                 reduction.newval );

            ++stats.nsidechgs;
            break;
         case RowReduction::RHS:
            assert( rflags[reduction.row].test( RowFlag::kRhsInf ) ||
                    reduction.newval !=
                        constraintMatrix.getRightHandSides()[reduction.row] );
            setRowState( reduction.row, State::kBoundsModified );
            if( rflags[reduction.row].test( RowFlag::kRhsInf ) )
            {
               auto rowvec =
                   constraintMatrix.getRowCoefficients( reduction.row );
               const int rowlen = rowvec.getLength();
               const int* rowcols = rowvec.getIndices();
               for( int i = 0; i != rowlen; ++i )
                  setColState( rowcols[i], State::kModified );
            }

            constraintMatrix.modifyRightHandSide( reduction.row,
                                                  reduction.newval );

            ++stats.nsidechgs;
            break;
         case RowReduction::LHS_INF:
            if( !rflags[reduction.row].test( RowFlag::kLhsInf ) )
            {
               setRowState( reduction.row, State::kBoundsModified );

               constraintMatrix.template modifyLeftHandSide<true>(
                   reduction.row, REAL{ 0 } );

               ++stats.nsidechgs;
            }
            break;
         case RowReduction::RHS_INF:
            if( !rflags[reduction.row].test( RowFlag::kRhsInf ) )
            {
               setRowState( reduction.row, State::kBoundsModified );
               constraintMatrix.template modifyRightHandSide<true>(
                   reduction.row, REAL{ 0 } );
               ++stats.nsidechgs;
            }
            break;
         case RowReduction::REDUNDANT:
            if( !rflags[reduction.row].test( RowFlag::kRedundant ) )
            {
               setRowState( reduction.row, State::kBoundsModified );
               markRowRedundant( reduction.row );
            }
            break;
         case RowReduction::SPARSIFY:
         {
            int nsparsifyrows = static_cast<int>( reduction.newval );
            int eqrow = reduction.row;
            assert( matrix_buffer.empty() );

            int ncancel = 0;
            int ncanceledrows = 0;

            auto eqrowvec = constraintMatrix.getRowCoefficients( eqrow );
            const REAL& eqrhs = constraintMatrix.getRightHandSides()[eqrow];
            int eqlen = eqrowvec.getLength();

            for( int i = 0; i != nsparsifyrows; ++i )
            {
               ++iter;
               int candrow = iter->row;
               const REAL& scale = iter->newval;

               assert( candrow != eqrow );

               int canceled = constraintMatrix.sparsify(
                   num, eqrow, scale, candrow, intbuffer, realbuffer,
                   problem.getVariableDomains(), changed_activities,
                   problem.getRowActivities(), singletonRows, singletonColumns,
                   emptyColumns, stats.nrounds );

               if( canceled != 0 )
               {
                  setRowState( candrow, State::kModified );
                  ++ncanceledrows;
                  ncancel += canceled;

                  if( eqrhs != 0 )
                  {
                     if( !rflags[candrow].test( RowFlag::kLhsInf ) )
                        ++stats.nsidechgs;

                     if( !rflags[candrow].test( RowFlag::kRhsInf ) )
                        ++stats.nsidechgs;
                  }
               }
            }

            if( ncancel != 0 )
            {
               stats.ncoefchgs += eqlen * ncanceledrows;
               const int* eqrowcols = eqrowvec.getIndices();

               for( int j = 0; j != eqlen; ++j )
                  setColState( eqrowcols[j], State::kModified );
            }
         }
         break;
         default:
            break;
         }
      }
   }

   // no conflicts found
   return ApplyResult::kApplied;
}

} // namespace papilo

#endif
