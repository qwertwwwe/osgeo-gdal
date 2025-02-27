/******************************************************************************
 *
 * Project:  R Format Driver
 * Purpose:  CreateCopy() implementation for R stats package object format.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2009, Frank Warmerdam <warmerdam@pobox.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include "cpl_port.h"
#include "rdataset.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_progress.h"
#include "cpl_string.h"
#include "cpl_vsi.h"
#include "gdal.h"
#include "gdal_pam.h"
#include "gdal_priv.h"


GDALDataset *
RCreateCopy( const char * pszFilename, GDALDataset *poSrcDS,
             int bStrict, char ** papszOptions,
             GDALProgressFunc pfnProgress, void * pProgressData );

/************************************************************************/
/* ==================================================================== */
/*                        Writer Implementation                         */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                           RWriteInteger()                            */
/************************************************************************/

static void RWriteInteger( VSILFILE *fp, int bASCII, int nValue )

{
    if( bASCII )
    {
        char szOutput[50] = { '\0' };
        snprintf(szOutput, sizeof(szOutput), "%d\n", nValue);
        VSIFWriteL(szOutput, 1, strlen(szOutput), fp);
    }
    else
    {
        CPL_MSBPTR32(&nValue);
        VSIFWriteL(&nValue, 4, 1, fp);
    }
}

/************************************************************************/
/*                            RWriteString()                            */
/************************************************************************/

static void RWriteString( VSILFILE *fp, int bASCII, const char *pszValue )

{
    RWriteInteger(fp, bASCII, 4105);
    RWriteInteger(fp, bASCII, static_cast<int>(strlen(pszValue)));

    if( bASCII )
    {
        VSIFWriteL(pszValue, 1, strlen(pszValue), fp);
        VSIFWriteL("\n", 1, 1, fp);
    }
    else
    {
        VSIFWriteL(pszValue, 1, static_cast<int>(strlen(pszValue)), fp);
    }
}

/************************************************************************/
/*                            RCreateCopy()                             */
/************************************************************************/

GDALDataset *
RCreateCopy( const char * pszFilename,
             GDALDataset *poSrcDS,
             CPL_UNUSED int bStrict,
             char ** papszOptions,
             GDALProgressFunc pfnProgress,
             void * pProgressData )
{
    const int nBands = poSrcDS->GetRasterCount();
    const int nXSize = poSrcDS->GetRasterXSize();
    const int nYSize = poSrcDS->GetRasterYSize();
    const bool bASCII = CPLFetchBool(papszOptions, "ASCII", false);
    const bool bCompressed = CPLFetchBool(papszOptions, "COMPRESS", !bASCII);

    vsi_l_offset nSize = static_cast<vsi_l_offset>(nBands) * nXSize * nYSize;
    if( nSize > static_cast<vsi_l_offset>(INT_MAX) )
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Too big raster");
        return nullptr;
    }

    // Some some rudimentary checks.

    // Setup the filename to actually use.  We prefix with
    // /vsigzip/ if we want compressed output.
    const CPLString osAdjustedFilename =
        std::string(bCompressed ? "/vsigzip/" : "") + pszFilename;

    // Create the file.
    VSILFILE *fp = VSIFOpenL(osAdjustedFilename, "wb");
    if( fp == nullptr )
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Unable to create file %s.",
                 pszFilename);
        return nullptr;
    }

    // Write header with version, etc.
    if( bASCII )
    {
        const char *pszHeader = "RDA2\nA\n";
        VSIFWriteL(pszHeader, 1, strlen(pszHeader), fp);
    }
    else
    {
        const char *pszHeader = "RDX2\nX\n";
        VSIFWriteL(pszHeader, 1, strlen(pszHeader), fp);
    }

    RWriteInteger(fp, bASCII, 2);
    RWriteInteger(fp, bASCII, 133377);
    RWriteInteger(fp, bASCII, 131840);

    // Establish the primary pairlist with one component object.
    RWriteInteger(fp, bASCII, 1026);
    RWriteInteger(fp, bASCII, 1);

    // Write the object name.  Eventually we should derive this
    // from the filename, possible with override by a creation option.
    RWriteString(fp, bASCII, "gg");

    // For now we write the raster as a numeric array with attributes (526).
    RWriteInteger(fp, bASCII, 526);
    RWriteInteger(fp, bASCII, nXSize * nYSize * nBands);

    // Write the raster data.
    CPLErr eErr = CE_None;

    double *padfScanline =
        static_cast<double *>(CPLMalloc(nXSize * sizeof(double)));

    for( int iBand = 0; iBand < nBands; iBand++ )
    {
        GDALRasterBand *poBand = poSrcDS->GetRasterBand(iBand + 1);

        for( int iLine = 0; iLine < nYSize && eErr == CE_None; iLine++ )
        {
            eErr = poBand->RasterIO(GF_Read, 0, iLine, nXSize, 1,
                                    padfScanline, nXSize, 1, GDT_Float64,
                                    sizeof(double), 0, nullptr);

            if( bASCII )
            {
                for( int iValue = 0; iValue < nXSize; iValue++ )
                {
                    char szValue[128] = { '\0' };
                    CPLsnprintf(szValue, sizeof(szValue), "%.16g\n",
                                padfScanline[iValue]);
                    VSIFWriteL(szValue, 1, strlen(szValue), fp);
                }
            }
            else
            {
                for( int iValue = 0; iValue < nXSize; iValue++ )
                    CPL_MSBPTR64(padfScanline + iValue);

                VSIFWriteL(padfScanline, 8, nXSize, fp);
            }

            if( eErr == CE_None &&
                !pfnProgress((iLine + 1) / static_cast<double>(nYSize),
                             nullptr, pProgressData) )
            {
                eErr = CE_Failure;
                CPLError(CE_Failure, CPLE_UserInterrupt,
                         "User terminated CreateCopy()");
            }
        }
    }

    CPLFree(padfScanline);

    // Write out the dims attribute.
    RWriteInteger(fp, bASCII, 1026);
    RWriteInteger(fp, bASCII, 1);

    RWriteString(fp, bASCII, "dim");

    RWriteInteger(fp, bASCII, 13);
    RWriteInteger(fp, bASCII, 3);
    RWriteInteger(fp, bASCII, nXSize);
    RWriteInteger(fp, bASCII, nYSize);
    RWriteInteger(fp, bASCII, nBands);

    RWriteInteger(fp, bASCII, 254);

    // Terminate overall pairlist.
    RWriteInteger(fp, bASCII, 254);

    // Cleanup.
    VSIFCloseL(fp);

    if( eErr != CE_None )
        return nullptr;

    // Re-open dataset, and copy any auxiliary pam information.
    GDALPamDataset *poDS =
        static_cast<GDALPamDataset *>(GDALOpen(pszFilename, GA_ReadOnly));

    if( poDS )
        poDS->CloneInfo(poSrcDS, GCIF_PAM_DEFAULT);

    return poDS;
}
