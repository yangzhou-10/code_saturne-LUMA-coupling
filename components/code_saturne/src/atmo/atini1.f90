!-------------------------------------------------------------------------------

! This file is part of Code_Saturne, a general-purpose CFD tool.
!
! Copyright (C) 1998-2021 EDF S.A.
!
! This program is free software; you can redistribute it and/or modify it under
! the terms of the GNU General Public License as published by the Free Software
! Foundation; either version 2 of the License, or (at your option) any later
! version.
!
! This program is distributed in the hope that it will be useful, but WITHOUT
! ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
! FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
! details.
!
! You should have received a copy of the GNU General Public License along with
! this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
! Street, Fifth Floor, Boston, MA 02110-1301, USA.

!-------------------------------------------------------------------------------
!> \file atini1.f90
!> \brief Initialisation of variable options for the atmospheric module in
!>      addition to what is done in usipsu function
!>
!> Warning some initialisations are done twice ...

subroutine atini1

!===============================================================================
! Module files
!===============================================================================

use paramx
use dimens
use numvar
use optcal
use cstphy
use entsor
use cstnum
use ppppar
use ppthch
use ppincl
use atincl
use atsoil
use atchem
use atimbr
use field
use cs_c_bindings

!===============================================================================

implicit none

! Local variables

integer          ii, isc, jj
double precision turb_schmidt
double precision visls_0

!===============================================================================

!===============================================================================
! 1. VERIFICATIONS
!===============================================================================

if (ippmod(iatmos).le.1) then
  if (iatra1.eq.1.or.iatsoil.eq.1) then
    write(nfecra, 1003)
    call csexit(1)
  endif
endif

!===============================================================================
! 2. Transported variables for IPPMOD(IATMOS) = 0, 1 or 2
!===============================================================================

! 2.0  Constant density
! =====================

if (ippmod(iatmos).eq.0) then

  ! constant density
  irovar = 0

  ! physical or numerical quantities specific to scalars

  do isc = 1, nscapp

    jj = iscapp(isc)

    if (iscavr(jj).le.0) then
      call field_set_key_double(ivarfl(isca(jj)), kvisl0, viscl0)
    endif

  enddo

endif

! 2.1  Dry atmosphere
! ===================

if (ippmod(iatmos).eq.1) then

  ! for the dry atmosphere case, non constant density
  irovar = 1

  ! physical or numerical quantities specific to scalars

  do isc = 1, nscapp

    jj = iscapp(isc)

    if (iscavr(jj).le.0) then
      call field_get_key_double(ivarfl(isca(jj)), kvisl0, visls_0)
      ! If not set elsewhere (user, GUI, ...)
      if (visls_0.lt.-grand) then

        ! For the temperature, the diffusivity factor is directly the thermal conductivity
        ! lambda = Cp * mu / Pr
        ! where Pr is the (molecular) Prandtl number
        if (itherm .eq. 1.and.jj.eq.iscalt) then
          visls_0 = viscl0 * cp0
        else
          visls_0 = viscl0
        endif
        call field_set_key_double(ivarfl(isca(jj)), kvisl0, visls_0)
      endif
    endif

  enddo

endif

! 2.2  Humid atmosphere
! =====================

if (ippmod(iatmos).eq.2) then

  ! for the humid atmosphere case, non constant density
  irovar = 1

  ! physical or numerical quantities specific to scalars

  do isc = 1, nscapp

    jj = iscapp(isc)

    if (iscavr(jj).le.0) then
      call field_set_key_double(ivarfl(isca(jj)), kvisl0, viscl0)
    endif

  enddo

endif

!===============================================================================
! 5. Turbulent Schmidt and Prandtl number for atmospheric flows
!===============================================================================

if (nscal.gt.0) then
  do ii = 1, nscal
    turb_schmidt = 0.7d0
    call field_set_key_double(ivarfl(isca(ii)), ksigmas, turb_schmidt)
  enddo
endif

!===============================================================================
! 6. Force Rij Matrix stabilisation for all atmospheric models
!===============================================================================

if (itytur.eq.3) irijnu = 1

!===============================================================================
! 7. Some initialization for meteo...
!===============================================================================

if (ippmod(iatmos).ge.0) then

  call init_meteo

  if (imbrication_flag) then
    call activate_imbrication
  endif

  call cs_at_data_assim_build_ops

  if (ifilechemistry.ge.1) then
    call init_chemistry
  endif

endif


!--------
! Formats
!--------

 1003 format(                                                     &
'@                                                            ',/,&
'@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@',/,&
'@                                                            ',/,&
'@ @@  WARNING:   STOP WHILE READING INPUT DATA               ',/,&
'@    =========                                               ',/,&
'@                ATMOSPHERIC  MODULE                         ',/,&
'@                                                            ',/,&
'@  Ground model (iatsoil) and radiative model (iatra1)       ',/,&
'@   are only available with humid atmosphere module          ',/,&
'@   (ippmod(iatmos) = 2).                                    ',/,&
'@  Computation CAN NOT run.                                  ',/,&
'@                                                            ',/,&
'@  Check the input data given through the User Interface     ',/,&
'@   or in cs_user_parameters.f90.                            ',/,&
'@                                                            ',/,&
'@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@',/,&
'@                                                            ',/)

!----
! End
!----

return
end subroutine atini1
