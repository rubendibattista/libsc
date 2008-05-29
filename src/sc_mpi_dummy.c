/*
  This file is part of the SC Library.
  The SC library provides support for parallel scientific applications.

  Copyright (C) 2008 Carsten Burstedde, Lucas Wilcox.

  The SC Library is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  The SC Library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with the SC Library.  If not, see <http://www.gnu.org/licenses/>.
*/

/* sc.h comes first in every compilation unit */
#include <sc.h>
#include <sc_mpi_dummy.h>

/* gettimeofday is in either of these two */
#ifdef SC_HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef SC_HAVE_TIME_H
#include <time.h>
#endif

static inline       size_t
mpi_dummy_sizeof (MPI_Datatype t)
{
  switch (t) {
  case MPI_CHAR:
  case MPI_SIGNED_CHAR:
  case MPI_UNSIGNED_CHAR:
    return sizeof (char);
  case MPI_BYTE:
    return 1;
  case MPI_SHORT:
  case MPI_UNSIGNED_SHORT:
    return sizeof (short);
  case MPI_INT:
  case MPI_UNSIGNED:
    return sizeof (int);
  case MPI_LONG:
  case MPI_UNSIGNED_LONG:
    return sizeof (long);
  case MPI_FLOAT:
    return sizeof (float);
  case MPI_DOUBLE:
    return sizeof (double);
  case MPI_LONG_DOUBLE:
    return sizeof (long double);
  case MPI_LONG_LONG_INT:
  case MPI_UNSIGNED_LONG_LONG:
    return sizeof (long long);
  default:
    SC_CHECK_NOT_REACHED ();
  }
}

static inline void
mpi_dummy_assert_op (MPI_Op op)
{
  switch (op) {
  case MPI_MAX:
  case MPI_MIN:
  case MPI_SUM:
  case MPI_PROD:
  case MPI_LAND:
  case MPI_BAND:
  case MPI_LOR:
  case MPI_BOR:
  case MPI_LXOR:
  case MPI_BXOR:
  case MPI_MINLOC:
  case MPI_MAXLOC:
  case MPI_REPLACE:
    break;
  default:
    SC_CHECK_NOT_REACHED ();
  }
}

int
MPI_Init (int *argc, char ***argv)
{
  return MPI_SUCCESS;
}

int
MPI_Finalize (void)
{
  return MPI_SUCCESS;
}

int
MPI_Abort (MPI_Comm comm, int exitcode)
{
  abort ();
}

int
MPI_Comm_size (MPI_Comm comm, int *size)
{
  *size = 1;

  return MPI_SUCCESS;
}

int
MPI_Comm_rank (MPI_Comm comm, int *rank)
{
  *rank = 0;

  return MPI_SUCCESS;
}

int
MPI_Barrier (MPI_Comm comm)
{
  return MPI_SUCCESS;
}

int
MPI_Bcast (void *p, int n, MPI_Datatype t, int rank, MPI_Comm comm)
{
  SC_ASSERT (rank == 0);

  return MPI_SUCCESS;
}

int
MPI_Gather (void *p, int np, MPI_Datatype tp,
            void *q, int nq, MPI_Datatype tq, int rank, MPI_Comm comm)
{
  size_t              lp, lq;

  SC_ASSERT (rank == 0 && np >= 0 && nq >= 0);

/* *INDENT-OFF* horrible indent bug */
  lp = (size_t) np * mpi_dummy_sizeof (tp);
  lq = (size_t) nq * mpi_dummy_sizeof (tq);
/* *INDENT-ON* */

  SC_ASSERT (lp == lq);
  memcpy (q, p, lp);

  return MPI_SUCCESS;
}

int
MPI_Allgather (void *p, int np, MPI_Datatype tp,
               void *q, int nq, MPI_Datatype tq, MPI_Comm comm)
{
  size_t              lp, lq;

  SC_ASSERT (np >= 0 && nq >= 0);

/* *INDENT-OFF* horrible indent bug */
  lp = (size_t) np * mpi_dummy_sizeof (tp);
  lq = (size_t) nq * mpi_dummy_sizeof (tq);
/* *INDENT-ON* */

  SC_ASSERT (lp == lq);
  memcpy (q, p, lp);

  return MPI_SUCCESS;
}

int
MPI_Reduce (void *p, void *q, int n, MPI_Datatype t,
            MPI_Op op, int rank, MPI_Comm comm)
{
  size_t              l;

  SC_ASSERT (rank == 0 && n >= 0);
  mpi_dummy_assert_op (op);

/* *INDENT-OFF* horrible indent bug */
  l = (size_t) n * mpi_dummy_sizeof (t);
/* *INDENT-ON* */

  memcpy (q, p, l);

  return MPI_SUCCESS;
}

int
MPI_Allreduce (void *p, void *q, int n, MPI_Datatype t,
               MPI_Op op, MPI_Comm comm)
{
  size_t              l;

  SC_ASSERT (n >= 0);
  mpi_dummy_assert_op (op);

/* *INDENT-OFF* horrible indent bug */
  l = (size_t) n * mpi_dummy_sizeof (t);
/* *INDENT-ON* */

  memcpy (q, p, l);

  return MPI_SUCCESS;
}

double
MPI_Wtime (void)
{
  int                 retval;
  struct timeval      tv;

  retval = gettimeofday (&tv, NULL);
  SC_CHECK_ABORT (retval == 0, "gettimeofday");

  return (double) tv.tv_sec + 1.e-6 * tv.tv_usec;
}

/* EOF sc_mpi_dummy.c */
