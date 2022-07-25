// Copyright CERN and copyright holders of ALICE O2. This software is
// // distributed under the terms of the GNU General Public License v3 (GPL
// // Version 3), copied verbatim in the file "COPYING".
// //
// // See http://alice-o2.web.cern.ch/license for full licensing information.
// //
// // In applying this license CERN does not waive the privileges and immunities
// // granted to it by virtue of its status as an Intergovernmental Organization
// // or submit itself to any jurisdiction.
//

///
/// \file   ITSClusterTask.cxx
/// \author Artem Isakov
/// \author Antonio Palasciano
///

#include "QualityControl/QcInfoLogger.h"
#include "ITS/ITSClusterTask.h"

#include <sstream>
#include <TCanvas.h>
#include <DataFormatsParameters/GRPObject.h>
#include <ITSMFTReconstruction/DigitPixelReader.h>
#include <DataFormatsITSMFT/ROFRecord.h>
#include <ITSMFTReconstruction/ChipMappingITS.h>
#include "ITSMFTReconstruction/ClustererParam.h"
#include "DetectorsCommonDataFormats/DetectorNameConf.h"
#include "ITStracking/IOUtils.h"
#include <DataFormatsITSMFT/ClusterTopology.h>
#include "CCDB/BasicCCDBManager.h"
#include "CCDB/CCDBTimeStampUtils.h"
#include <Framework/InputRecord.h>
#include <THnSparse.h>

#ifdef WITH_OPENMP
#include <omp.h>
#endif

using o2::itsmft::Digit;
using namespace o2::itsmft;
using namespace o2::its;

namespace o2::quality_control_modules::its
{

ITSClusterTask::ITSClusterTask() : TaskInterface() {}

ITSClusterTask::~ITSClusterTask()
{
  delete hClusterVsBunchCrossing;
  delete hClusterPerChip;
  delete hVerticesRof;
  delete hClusterPerChip_Scaled;
  delete hClusterPerChip_Weigth;
  for (Int_t iLayer = 0; iLayer < NLayer; iLayer++) {

    if (!mEnableLayers[iLayer])
      continue;

    // delete sClustersSize[iLayer];
    delete hClusterSizeLayerSummary[iLayer];
    delete hClusterTopologyLayerSummary[iLayer];
    delete hGroupedClusterSizeLayerSummary[iLayer];

    if (iLayer < 3) {

      delete hAverageClusterOccupancySummaryIB[iLayer];
      delete hAverageClusterOccupancyMonitorIB[iLayer];
      delete hAverageClusterSizeSummaryIB[iLayer];
      delete hAverageClusterSizeMonitorIB[iLayer];

      for (Int_t iStave = 0; iStave < mNStaves[iLayer]; iStave++) {
        for (Int_t iChip = 0; iChip < mNChipsPerHic[iLayer]; iChip++) {

          delete hClusterTopologySummaryIB[iLayer][iStave][iChip];
          delete hGroupedClusterSizeSummaryIB[iLayer][iStave][iChip];
        }
      }
    } else {

      delete hAverageClusterOccupancySummaryOB[iLayer];
      delete hAverageClusterOccupancyMonitorOB[iLayer];
      delete hAverageClusterSizeSummaryOB[iLayer];
      delete hAverageClusterSizeMonitorOB[iLayer];

      for (Int_t iStave = 0; iStave < mNStaves[iLayer]; iStave++) {

        delete hClusterSizeSummaryOB[iLayer][iStave];
        delete hClusterTopologySummaryOB[iLayer][iStave];
        delete hGroupedClusterSizeSummaryOB[iLayer][iStave];
      }
    }
  }
}

void ITSClusterTask::initialize(o2::framework::InitContext& /*ctx*/)
{

  ILOG(Info, Support) << "initialize ITSClusterTask" << ENDM;

  getJsonParameters();

  o2::base::GeometryManager::loadGeometry(mGeomPath.c_str());
  mGeom = o2::its::GeometryTGeo::Instance();

  createAllHistos();

  mGeneralOccupancy = new TH2D("General/General_Occupancy", "General Cluster Occupancy (max n_clusters/event/chip)", 24, -12, 12, 14, 0, 14);

  addObject(mGeneralOccupancy);
  mGeneralOccupancy->SetStats(0);
  mGeneralOccupancy->SetBit(TH1::kIsAverage);
  for (int iy = 1; iy <= mGeneralOccupancy->GetNbinsY(); iy++) {
    mGeneralOccupancy->GetYaxis()->SetBinLabel(iy, mYlabels[iy - 1].c_str());
  }
  mGeneralOccupancy->GetXaxis()->SetLabelOffset(999);
  mGeneralOccupancy->GetXaxis()->SetLabelSize(0);
  mGeneralOccupancy->GetZaxis()->SetTitle("Max Avg Cluster occ (clusters/event/chip)");
  publishHistos();

  // get dict from ccdb
  mTimestamp = std::stol(mCustomParameters["dicttimestamp"]);
  long int ts = mTimestamp ? mTimestamp : o2::ccdb::getCurrentTimestamp();
  ILOG(Info, Support) << "Getting dictionary from ccdb - timestamp: " << ts << ENDM;
  auto& mgr = o2::ccdb::BasicCCDBManager::instance();
  mgr.setTimestamp(ts);
  mDict = mgr.get<o2::itsmft::TopologyDictionary>("ITS/Calib/ClusterDictionary");
  ILOG(Info, Support) << "Dictionary size: " << mDict->getSize() << ENDM;
}

void ITSClusterTask::startOfActivity(Activity& /*activity*/)
{
  ILOG(Info, Support) << "startOfActivity" << ENDM;
  reset();
}

void ITSClusterTask::startOfCycle()
{
  ILOG(Info, Support) << "startOfCycle" << ENDM;
}

void ITSClusterTask::monitorData(o2::framework::ProcessingContext& ctx)
{

  std::chrono::time_point<std::chrono::high_resolution_clock> start;
  std::chrono::time_point<std::chrono::high_resolution_clock> end;
  int difference;
  start = std::chrono::high_resolution_clock::now();

  ILOG(Info, Support) << "START DOING QC General" << ENDM;
  auto clusArr = ctx.inputs().get<gsl::span<o2::itsmft::CompClusterExt>>("compclus");
  auto clusRofArr = ctx.inputs().get<gsl::span<o2::itsmft::ROFRecord>>("clustersrof");
  auto clusPatternArr = ctx.inputs().get<gsl::span<unsigned char>>("patterns");
  auto vertexArr = ctx.inputs().get<gsl::span<o2::dataformats::Vertex<o2::dataformats::TimeStamp<int>>>>("Vertices");
  auto vertexRofArr = ctx.inputs().get<gsl::span<o2::itsmft::ROFRecord>>("Verticesrof");
  auto pattIt = clusPatternArr.begin();
  int dictSize = mDict->getSize();

  int iPattern = 0;
  int ChipIDprev = -1;
#ifdef WITH_OPENMP
  omp_set_num_threads(mNThreads);
#pragma omp parallel for schedule(dynamic)
#endif
//Doing something new here!!!
for (int iROF = 0; iROF < vertexRofArr.size(); iROF++) {

    int start = vertexRofArr[iROF].getFirstEntry();
    int end = start + vertexRofArr[iROF].getNEntries();
    int nvtxROF = 0;
    int nvtxROF_nocut = vertexRofArr[iROF].getNEntries();
    int rofFLAG = 1;
    double zVertexAverage = 0;
    double xVertexAverage = 0;
    double yVertexAverage = 0;
    for (int ivtx = start; ivtx < end; ivtx++) {
      auto& vertex = vertexArr[ivtx];
      if (vertex.getNContributors() > 2) {
        nvtxROF++;
      }
      for (int jvtx = start; jvtx < end; jvtx++) {
        if(jvtx == ivtx) continue;
        auto& jvertex = vertexArr[jvtx];
        if(jvertex.getNContributors() < 2) continue;
        //if(std::abs(jvertex.getX() - vertex.getX()) > 1) rofFLAG = 0;
        //if(std::abs(jvertex.getY() - vertex.getY()) > 1) rofFLAG = 0;
        if(std::abs(jvertex.getZ() - vertex.getZ()) > 1) rofFLAG = 0;
      }
      zVertexAverage += vertex.getZ();
      yVertexAverage += vertex.getY();
      xVertexAverage += vertex.getX();
    }
    if(nvtxROF ==0) continue;
    zVertexAverage /= nvtxROF;  xVertexAverage /= nvtxROF;  yVertexAverage /= nvtxROF;
    o2::math_utils::Point3D<float> locC;
    hVerticesRof->Fill(nvtxROF_nocut);
    if (rofFLAG){
      const auto& ROF = clusRofArr[iROF];
      for (int icl = ROF.getFirstEntry(); icl < ROF.getFirstEntry() + ROF.getNEntries(); icl++) {
        auto& cluster = clusArr[icl];
        auto ChipID = cluster.getSensorID();
        int lay, sta, ssta, mod, chip, lane;
        if (ChipID != ChipIDprev) {
          mGeom->getChipId(ChipID, lay, sta, ssta, mod, chip);
          mod = mod + (ssta * (mNHicPerStave[lay] / 2));
          int chipIdLocal = (ChipID - ChipBoundary[lay]) % (14 * mNHicPerStave[lay]);
          lane = (chipIdLocal % (14 * mNHicPerStave[lay])) / (14 / 2);
        }
        if (lay < 3) {
          auto pattID = cluster.getPatternID();
          if (pattID == o2::itsmft::CompCluster::InvalidPatternID || mDict->isGroup(pattID)) {
            o2::itsmft::ClusterPattern patt(pattIt);
            locC = mDict->getClusterCoordinates(cluster, patt, false);
          } else {
            locC = mDict->getClusterCoordinates(cluster);
            //errX = dict->getErrX(pattID);
            //errZ = dict->getErrZ(pattID);
          }
           // Transformation to the local --> global
          mGeom->fillMatrixCache(o2::math_utils::bit2Mask(o2::math_utils::TransformType::L2G));
          const math_utils::Point3D<float> loc(0., 0., 0.);
          o2::math_utils::Point3D<float> gloC = mGeom->getMatrixL2G(ChipID)(loc);
          o2::math_utils::Point3D<float> gloCpos = mGeom->getMatrixL2G(ChipID)(locC);
          //ILOG(Info, Support) << "ChipID " << ChipID << "Chip " << chip << " Layer " << lay << " Stave " << sta << ENDM;
          hClusterPerChip->Fill(ChipID);
          hClusterPerChip_Scaled->Fill(ChipID);
          //ILOG(Info, Support) << "ChipID " << ChipID << "Chip " << chip << " Layer " << lay << " Stave " << sta << " X "<<  gloC.X() << ENDM;
          double deltaZ = std::abs(gloCpos.Z()-zVertexAverage);
          double deltaR = std::sqrt(std::pow(gloCpos.X()-xVertexAverage,2)+std::pow(gloCpos.Y()-yVertexAverage,2));
          double weight = deltaR*std::sqrt(deltaZ*deltaZ + std::pow(deltaR,2)); //1
          /* ILOG(Info, Support) << "loc  ->  " << "Pos: X=" << locC.X()<< "Y =" << locC.Y() << " Z=" << locC.Z() << " Weight= " << weight << ENDM;
          ILOG(Info, Support) << "gloc(0,0,0)  ->  " << "Pos: X=" << gloC.X()<< "Y =" << gloC.Y() << " Z=" << gloC.Z() << " Weight= " << weight << ENDM;
          ILOG(Info, Support) << "gloc(0,0,0)  ->  " << "Pos: X=" << gloCpos.X()<< "Y =" << gloCpos.Y() << " Z=" << gloCpos.Z() << " Weight= " << weight << ENDM;
          ILOG(Info, Support) << ENDM; */
          hClusterPerChip_Weigth->Fill(ChipID, 1/weight);
        } //end inner barrel
      }   //end loop on clusters
    } //end rofFLAG
    //Scaling per # of vertex per ROF and multiplying per chip dimensions(in cm)
      //hClusterPerChip_Scaled = (TH1D*) hClusterPerChip->Clone();
    ILOG(Info, Support) << "VertexROF= " << nvtxROF << "NVertex ROF no cut " << nvtxROF_nocut << ENDM;
    //hClusterPerChip_Scaled->Scale(1/nvtxROF * xDimensionChip * zDimensionChip);
    //hClusterPerChip_Weigth->Scale(1/nvtxROF * xDimensionChip * zDimensionChip);
}

  // Filling cluster histogram for each ROF by open_mp
  //ILOG(Info, Support) << "1Check" << ENDM;

  for (unsigned int iROF = 0; iROF < clusRofArr.size(); iROF++) {

    const auto& ROF = clusRofArr[iROF];
    const auto bcdata = ROF.getBCData();
    //ILOG(Info, Support) << "2Check" << ENDM; //it's ok here

    int nClustersForBunchCrossing = 0;
    for (int icl = ROF.getFirstEntry(); icl < ROF.getFirstEntry() + ROF.getNEntries(); icl++) {

      auto& cluster = clusArr[icl];
      auto ChipID = cluster.getSensorID();
      int ClusterID = cluster.getPatternID(); // used for normal (frequent) cluster shapes
      int lay, sta, ssta, mod, chip, lane;

      if (ChipID != ChipIDprev) {
        mGeom->getChipId(ChipID, lay, sta, ssta, mod, chip);
        mod = mod + (ssta * (mNHicPerStave[lay] / 2));
        int chipIdLocal = (ChipID - ChipBoundary[lay]) % (14 * mNHicPerStave[lay]);
        lane = (chipIdLocal % (14 * mNHicPerStave[lay])) / (14 / 2);
      }
      //ILOG(Info, Support) << "3Check" << ENDM; //it's ok here

      int npix = -1;
      int isGrouped = -1;
      if (ClusterID != o2::itsmft::CompCluster::InvalidPatternID && !mDict->isGroup(ClusterID)) { // Normal (frequent) cluster shapes
        npix = mDict->getNpixels(ClusterID);
        isGrouped = 0;
      } else {
        o2::itsmft::ClusterPattern patt(pattIt);
        npix = patt.getNPixels();
        isGrouped = 1;
      }

      if (npix > 2)
        nClustersForBunchCrossing++;

      if (lay < 3) {
        
        mClusterOccupancyIB[lay][sta][chip]++;
        mClusterOccupancyIBmonitor[lay][sta][chip]++;
        if (ClusterID < dictSize) {

          // Double_t ClusterSizeFill[3] = {1.*sta, 1.*chip,1.* mDict.getNpixels(ClusterID)};
          // sClustersSize[lay]->Fill(ClusterSizeFill, 1.);
          mClusterSize[lay][sta][chip] += npix;
          mClusterSizeMonitor[lay][sta][chip] += npix;
          nClusters[lay][sta][chip]++;

          //ILOG(Info, Support) << "4Check" << ENDM; //it's ok here
          hClusterTopologySummaryIB[lay][sta][chip]->Fill(ClusterID);

          hClusterSizeLayerSummary[lay]->Fill(npix);
          hClusterTopologyLayerSummary[lay]->Fill(ClusterID);

          if (isGrouped) {
            hGroupedClusterSizeSummaryIB[lay][sta][chip]->Fill(npix);
            hGroupedClusterSizeLayerSummary[lay]->Fill(npix);
          }
        }
      } else {

        mClusterOccupancyOB[lay][sta][lane]++;
        mClusterOccupancyOBmonitor[lay][sta][lane]++;
        if (ClusterID < dictSize) {
          // Double_t ClusterSizeFill[3] = {1.*sta, 1.*mod, 1.*mDict.getNpixels(ClusterID)};
          // sClustersSize[lay]->Fill(ClusterSizeFill, 1.);
          //ILOG(Info, Support) << "5Check" << ENDM; //Ok here
          mClusterSize[lay][sta][lane] += npix;
          mClusterSizeMonitor[lay][sta][lane] += npix;
          nClusters[lay][sta][lane]++;

          hClusterTopologySummaryOB[lay][sta]->Fill(ClusterID);
          hClusterSizeSummaryOB[lay][sta]->Fill(npix);
          hClusterSizeLayerSummary[lay]->Fill(npix);
          hClusterTopologyLayerSummary[lay]->Fill(ClusterID);
          if (isGrouped) {
            hGroupedClusterSizeSummaryOB[lay][sta]->Fill(npix);
            hGroupedClusterSizeLayerSummary[lay]->Fill(npix);
          }
        }
      }
    }
    //ILOG(Info, Support) << "6Check" << ENDM; //it's ok here//it's ok here
    hClusterVsBunchCrossing->Fill(bcdata.bc, nClustersForBunchCrossing); // we count only the number of clusters, not their sizes
    //ILOG(Info, Support) << "7Check" << ENDM; //it's ok here//it's ok here
  }
  //Identified problem here -> Where it does come from? 
  ILOG(Info, Support) << "Istheproblemhere?" << ENDM; //This is not printed out in the log file
  
  mNRofs += clusRofArr.size();        // USED to calculate occupancy for the whole run
  mNRofsMonitor += clusRofArr.size(); // Occupancy in the last N ROFs
  ILOG(Info, Support) << "LastBigLoop" << ENDM;

  if (mNRofs > 0) {
    for (Int_t iLayer = 0; iLayer < NLayer; iLayer++) {

      if (!mEnableLayers[iLayer])
        continue;

      for (Int_t iStave = 0; iStave < mNStaves[iLayer]; iStave++) {

        if (iLayer < 3) {
          for (Int_t iChip = 0; iChip < mNChipsPerHic[iLayer]; iChip++) {
            hAverageClusterOccupancySummaryIB[iLayer]->SetBinContent(iChip + 1, iStave + 1, 1. * mClusterOccupancyIB[iLayer][iStave][iChip] / mNRofs);
            hAverageClusterOccupancySummaryIB[iLayer]->SetBinError(iChip + 1, iStave + 1, 1e-15);
            if (nClusters[iLayer][iStave][iChip] != 0) {
              hAverageClusterSizeSummaryIB[iLayer]->SetBinContent(iChip + 1, iStave + 1, (double)mClusterSize[iLayer][iStave][iChip] / nClusters[iLayer][iStave][iChip]);
              hAverageClusterSizeSummaryIB[iLayer]->SetBinError(iChip + 1, iStave + 1, 1e-15);
            }
          }
          int ybin = iStave < (mNStaves[iLayer] / 2) ? 7 + iLayer + 1 : 7 - iLayer;
          int xbin = 12 - mNStaves[iLayer] / 4 + 1 + (iStave % (mNStaves[iLayer] / 2));
          mGeneralOccupancy->SetBinContent(xbin, ybin, (float)(*(std::max_element(mClusterOccupancyIB[iLayer][iStave], mClusterOccupancyIB[iLayer][iStave] + mNChipsPerHic[iLayer]))) / (float)(mNRofs));
          mGeneralOccupancy->SetBinError(xbin, ybin, 1e-15);
        } else {

          for (Int_t iLane = 0; iLane < mNLanePerHic[iLayer] * mNHicPerStave[iLayer]; iLane++) {
            hAverageClusterOccupancySummaryOB[iLayer]->SetBinContent(iLane + 1, iStave + 1, 1. * mClusterOccupancyOB[iLayer][iStave][iLane] / mNRofs / (mNChipsPerHic[iLayer] / mNLanePerHic[iLayer])); // 14 To have occupation per chip -> 7 because we're considering lanes
            hAverageClusterOccupancySummaryOB[iLayer]->SetBinError(iLane + 1, iStave + 1, 1e-15);                                                                                                       // 14 To have occupation per chip
            if (nClusters[iLayer][iStave][iLane] != 0) {
              hAverageClusterSizeSummaryOB[iLayer]->SetBinContent(iLane + 1, iStave + 1, (double)mClusterSize[iLayer][iStave][iLane] / nClusters[iLayer][iStave][iLane]);
              hAverageClusterSizeSummaryOB[iLayer]->SetBinError(iLane + 1, iStave + 1, 1e-15);
            }
          }
          int ybin = iStave < (mNStaves[iLayer] / 2) ? 7 + iLayer + 1 : 7 - iLayer;
          int xbin = 12 - mNStaves[iLayer] / 4 + 1 + (iStave % (mNStaves[iLayer] / 2));
          mGeneralOccupancy->SetBinContent(xbin, ybin, (float)(*(std::max_element(mClusterOccupancyOB[iLayer][iStave], mClusterOccupancyOB[iLayer][iStave] + mNHicPerStave[iLayer] * mNLanePerHic[iLayer]))) / (float)(mNRofs) / (mNChipsPerHic[iLayer] / mNLanePerHic[iLayer]));
          mGeneralOccupancy->SetBinError(xbin, ybin, 1e-15);
        }
      }
    }
  }

  ILOG(Info, Support) << "AfterFinishingPlotsFilling" << ENDM;
  if (mNRofsMonitor >= mOccUpdateFrequency) {
    updateOccMonitorPlots();
    mNRofsMonitor = 0;
    memset(mClusterOccupancyIBmonitor, 0, sizeof(mClusterOccupancyIBmonitor));
    memset(mClusterOccupancyOBmonitor, 0, sizeof(mClusterOccupancyOBmonitor));
  }

  end = std::chrono::high_resolution_clock::now();
  difference = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
  ILOG(Info, Support) << "Time in QC Cluster Task:  " << difference << ENDM;
}

void ITSClusterTask::updateOccMonitorPlots()
{

  for (Int_t iLayer = 0; iLayer < NLayer; iLayer++) {

    if (!mEnableLayers[iLayer])
      continue;

    for (Int_t iStave = 0; iStave < mNStaves[iLayer]; iStave++)
      if (iLayer < 3) {

        for (Int_t iChip = 0; iChip < mNChipsPerHic[iLayer]; iChip++) {
          hAverageClusterOccupancyMonitorIB[iLayer]->SetBinContent(iChip + 1, iStave + 1, 1. * mClusterOccupancyIBmonitor[iLayer][iStave][iChip] / mNRofsMonitor);
          hAverageClusterOccupancyMonitorIB[iLayer]->SetBinError(iChip + 1, iStave + 1, 1e-15);
          hAverageClusterSizeMonitorIB[iLayer]->SetBinContent(iChip + 1, iStave + 1, mClusterSizeMonitor[iLayer][iStave][iChip] / nClusters[iLayer][iStave][iChip]);
          hAverageClusterSizeMonitorIB[iLayer]->SetBinError(iChip + 1, iStave + 1, 1e-15);
        }
      } else {

        for (Int_t iLane = 0; iLane < mNLanePerHic[iLayer] * mNHicPerStave[iLayer]; iLane++) {
          hAverageClusterOccupancyMonitorOB[iLayer]->SetBinContent(iLane + 1, iStave + 1, 1. * mClusterOccupancyOBmonitor[iLayer][iStave][iLane] / mNRofsMonitor / (14 / 2)); // 7 To have occupation per chip
          hAverageClusterOccupancyMonitorOB[iLayer]->SetBinError(iLane + 1, iStave + 1, 1e-15);
          hAverageClusterSizeMonitorOB[iLayer]->SetBinContent(iLane + 1, iStave + 1, mClusterSizeMonitor[iLayer][iStave][iLane] / nClusters[iLayer][iStave][iLane]);
          hAverageClusterSizeMonitorOB[iLayer]->SetBinError(iLane + 1, iStave + 1, 1e-15);
        }
      }
  }
}

void ITSClusterTask::endOfCycle()
{
  ILOG(Info, Support) << "endOfCycle" << ENDM;
}

void ITSClusterTask::endOfActivity(Activity& /*activity*/)
{
  ILOG(Info, Support) << "endOfActivity" << ENDM;
}

void ITSClusterTask::reset()
{
  ILOG(Info, Support) << "Resetting the histogram" << ENDM;
  hClusterVsBunchCrossing->Reset();
  hClusterPerChip->Reset();
  hClusterPerChip_Scaled->Reset();
  hClusterPerChip_Weigth->Reset();
  hVerticesRof->Reset();
  mGeneralOccupancy->Reset();

  for (Int_t iLayer = 0; iLayer < NLayer; iLayer++) {
    if (!mEnableLayers[iLayer])
      continue;
    // sClustersSize[iLayer]->Reset();
    hClusterSizeLayerSummary[iLayer]->Reset();
    hGroupedClusterSizeLayerSummary[iLayer]->Reset();
    hClusterTopologyLayerSummary[iLayer]->Reset();

    if (iLayer < 3) {
      hAverageClusterOccupancySummaryIB[iLayer]->Reset();
      hAverageClusterOccupancyMonitorIB[iLayer]->Reset();
      hAverageClusterSizeSummaryIB[iLayer]->Reset();
      hAverageClusterSizeMonitorIB[iLayer]->Reset();
      for (Int_t iStave = 0; iStave < mNStaves[iLayer]; iStave++) {
        for (Int_t iChip = 0; iChip < mNChipsPerHic[iLayer]; iChip++) {
          hClusterTopologySummaryIB[iLayer][iStave][iChip]->Reset();
          hGroupedClusterSizeSummaryIB[iLayer][iStave][iChip]->Reset();
        }
      }
    } else {
      hAverageClusterOccupancySummaryOB[iLayer]->Reset();
      hAverageClusterSizeSummaryOB[iLayer]->Reset();
      hAverageClusterOccupancyMonitorOB[iLayer]->Reset();
      hAverageClusterSizeMonitorOB[iLayer]->Reset();
      for (Int_t iStave = 0; iStave < mNStaves[iLayer]; iStave++) {

        hClusterTopologySummaryOB[iLayer][iStave]->Reset();
        hGroupedClusterSizeSummaryOB[iLayer][iStave]->Reset();
      }
    }
  }
}

void ITSClusterTask::createAllHistos()
{

  hClusterVsBunchCrossing = new TH2D("BunchCrossingIDvsClusters", "BunchCrossingIDvsClusters", nBCbins, 0, 4095, 100, 0, 1000);
  hClusterVsBunchCrossing->SetTitle("#clusters vs BC id for clusters with npix > 2");
  addObject(hClusterVsBunchCrossing);
  formatAxes(hClusterVsBunchCrossing, "Bunch Crossing ID", "Number of clusters with npix > 2 in ROF", 1, 1.10);
  hClusterVsBunchCrossing->SetStats(0);

  hClusterPerChip = new TH1D("ClusterPerChip", "ClusterPerChip", 432, 0, 432);
  hClusterPerChip->SetTitle("#clusters per chip");
  addObject(hClusterPerChip);
  formatAxes(hClusterPerChip, "Chip", "Cluster", 1, 1.10);
  hClusterPerChip->SetStats(0);
 
  hClusterPerChip_Scaled = new TH1F("ClusterPerChip_ChipDimensionsScaled", "ClusterPerChip_ChipDimensionsScaled", 432, 0, 432);
  hClusterPerChip_Scaled->SetTitle("#clusters per chip scaled to dimension of the chip");
  addObject(hClusterPerChip_Scaled);
  formatAxes(hClusterPerChip_Scaled, "Chip", "Cluster", 1, 1.10);
  //hClusterPerChip_Scaled->SetStats(0);

  hClusterPerChip_Weigth = new TH1F("ClusterPerChip_W", "ClusterPerChip_W", 432, 0, 432);
  hClusterPerChip_Weigth->SetTitle("#clusters per chip weighted");
  addObject(hClusterPerChip_Weigth);
  formatAxes(hClusterPerChip_Weigth, "Chip", "Cluster", 1, 1.10);
  //hClusterPerChip_Weigth->SetStats(0);

  hVerticesRof = new TH1D("VerticesPerRof", "VerticesPerRof", 20, 0, 20);
  hVerticesRof->SetTitle("#VerticesPerRof");
  addObject(hVerticesRof);
  formatAxes(hVerticesRof, "", "vertices", 1, 1.10);
  //hVerticesRof->SetStats(0);

  for (Int_t iLayer = 0; iLayer < NLayer; iLayer++) {
    if (!mEnableLayers[iLayer])
      continue;

    hClusterSizeLayerSummary[iLayer] = new TH1D(Form("Layer%d/AverageClusterSizeSummary", iLayer), Form("Layer%dAverageClusterSizeSummary", iLayer), 100, 0, 100);
    hClusterSizeLayerSummary[iLayer]->SetTitle(Form("Cluster size summary for Layer %d", iLayer));
    addObject(hClusterSizeLayerSummary[iLayer]);
    formatAxes(hClusterSizeLayerSummary[iLayer], "Cluster Size (pixels)", "counts", 1, 1.10);
    hClusterSizeLayerSummary[iLayer]->SetStats(0);

    hGroupedClusterSizeLayerSummary[iLayer] = new TH1D(Form("Layer%d/AverageGroupedClusterSizeSummary", iLayer), Form("Layer%dAverageGroupedClusterSizeSummary", iLayer), 100, 0, 100);
    hGroupedClusterSizeLayerSummary[iLayer]->SetTitle(Form("Cluster size summary for Layer %d", iLayer));
    addObject(hGroupedClusterSizeLayerSummary[iLayer]);
    formatAxes(hGroupedClusterSizeLayerSummary[iLayer], "Grouped Cluster Size (pixels)", "counts", 1, 1.10);
    hGroupedClusterSizeLayerSummary[iLayer]->SetStats(0);

    hClusterTopologyLayerSummary[iLayer] = new TH1D(Form("Layer%d/ClusterTopologySummary", iLayer), Form("Layer%dClusterTopologySummary", iLayer), 300, 0, 300);
    hClusterTopologyLayerSummary[iLayer]->SetTitle(Form("Cluster topology summary for Layer %d", iLayer));
    addObject(hClusterTopologyLayerSummary[iLayer]);
    formatAxes(hClusterTopologyLayerSummary[iLayer], "Cluster Topology (ID)", "counts", 1, 1.10);
    hClusterTopologyLayerSummary[iLayer]->SetStats(0);

    if (iLayer < 3) {

      hAverageClusterOccupancySummaryIB[iLayer] = new TH2D(Form("Layer%d/ClusterOccupation", iLayer), Form("Layer%dClusterOccupancy", iLayer), mNChipsPerHic[iLayer], 0, mNChipsPerHic[iLayer], mNStaves[iLayer], 0, mNStaves[iLayer]);
      hAverageClusterOccupancySummaryIB[iLayer]->SetTitle(Form("Cluster Occupancy on Layer %d", iLayer));
      addObject(hAverageClusterOccupancySummaryIB[iLayer]);
      formatAxes(hAverageClusterOccupancySummaryIB[iLayer], "Chip Number", "Stave Number", 1, 1.10);
      hAverageClusterOccupancySummaryIB[iLayer]->SetStats(0);
      hAverageClusterOccupancySummaryIB[iLayer]->SetBit(TH1::kIsAverage);
      hAverageClusterOccupancySummaryIB[iLayer]->GetYaxis()->SetLabelSize(0.02);
      hAverageClusterOccupancySummaryIB[iLayer]->GetXaxis()->SetLabelSize(0.02);

      hAverageClusterOccupancyMonitorIB[iLayer] = new TH2D(Form("Layer%d/ClusterOccupationForLastNROFS", iLayer), Form("Layer%dClusterOccupancyForLastNROFS", iLayer), mNChipsPerHic[iLayer], 0, mNChipsPerHic[iLayer], mNStaves[iLayer], 0, mNStaves[iLayer]);
      hAverageClusterOccupancyMonitorIB[iLayer]->SetTitle(Form("Cluster Occupancy for the last NROFs on Layer %d", iLayer));
      addObject(hAverageClusterOccupancyMonitorIB[iLayer]);
      formatAxes(hAverageClusterOccupancyMonitorIB[iLayer], "Chip Number", "Stave Number", 1, 1.10);
      hAverageClusterOccupancyMonitorIB[iLayer]->SetStats(0);
      hAverageClusterOccupancyMonitorIB[iLayer]->SetBit(TH1::kIsAverage);
      hAverageClusterOccupancyMonitorIB[iLayer]->GetYaxis()->SetLabelSize(0.02);
      hAverageClusterOccupancyMonitorIB[iLayer]->GetXaxis()->SetLabelSize(0.02);

      hAverageClusterSizeSummaryIB[iLayer] = new TH2D(Form("Layer%d/AverageClusterSize", iLayer), Form("Layer%dAverageClusterSize", iLayer), mNChipsPerHic[iLayer], 0, mNChipsPerHic[iLayer], mNStaves[iLayer], 0, mNStaves[iLayer]);
      hAverageClusterSizeSummaryIB[iLayer]->SetTitle(Form("Average Cluster Size  on Layer %d", iLayer));
      addObject(hAverageClusterSizeSummaryIB[iLayer]);
      formatAxes(hAverageClusterSizeSummaryIB[iLayer], "Chip Number", "Stave Number", 1, 1.10);
      hAverageClusterSizeSummaryIB[iLayer]->SetStats(0);
      hAverageClusterSizeSummaryIB[iLayer]->SetBit(TH1::kIsAverage);
      hAverageClusterSizeSummaryIB[iLayer]->GetYaxis()->SetLabelSize(0.02);
      hAverageClusterSizeSummaryIB[iLayer]->GetXaxis()->SetLabelSize(0.02);

      hAverageClusterSizeMonitorIB[iLayer] = new TH2D(Form("Layer%d/AverageClusterSizeForLastNROFS", iLayer), Form("Layer%dAverageClusterSizeForLastNROFS", iLayer), mNChipsPerHic[iLayer], 0, mNChipsPerHic[iLayer], mNStaves[iLayer], 0, mNStaves[iLayer]);
      hAverageClusterSizeMonitorIB[iLayer]->SetTitle(Form("Average Cluster Size for the last NROFs on Layer %d", iLayer));
      addObject(hAverageClusterSizeMonitorIB[iLayer]);
      formatAxes(hAverageClusterSizeMonitorIB[iLayer], "Chip Number", "Stave Number", 1, 1.10);
      hAverageClusterSizeMonitorIB[iLayer]->SetStats(0);
      hAverageClusterSizeMonitorIB[iLayer]->SetBit(TH1::kIsAverage);
      hAverageClusterSizeMonitorIB[iLayer]->GetYaxis()->SetLabelSize(0.02);
      hAverageClusterSizeMonitorIB[iLayer]->GetXaxis()->SetLabelSize(0.02);

      for (Int_t iStave = 0; iStave < mNStaves[iLayer]; iStave++) {
        hAverageClusterSizeSummaryIB[iLayer]->GetYaxis()->SetBinLabel(iStave + 1, Form("Stave %i", iStave));
        hAverageClusterSizeMonitorIB[iLayer]->GetYaxis()->SetBinLabel(iStave + 1, Form("Stave %i", iStave));
        hAverageClusterOccupancySummaryIB[iLayer]->GetYaxis()->SetBinLabel(iStave + 1, Form("Stave %i", iStave));
        hAverageClusterOccupancyMonitorIB[iLayer]->GetYaxis()->SetBinLabel(iStave + 1, Form("Stave %i", iStave));

        for (Int_t iChip = 0; iChip < mNChipsPerHic[iLayer]; iChip++) {
          hGroupedClusterSizeSummaryIB[iLayer][iStave][iChip] = new TH1D(Form("Layer%d/Stave%d/CHIP%d/ClusterSizeGrouped", iLayer, iStave, iChip), Form("Layer%dStave%dCHIP%dClusterSizeGroped", iLayer, iStave, iChip), 100, 0, 100);
          hGroupedClusterSizeSummaryIB[iLayer][iStave][iChip]->SetTitle(Form("Cluster Size for grouped topologies on Layer %d Stave %d Chip %d", iLayer, iStave, iChip));
          addObject(hGroupedClusterSizeSummaryIB[iLayer][iStave][iChip]);
          formatAxes(hGroupedClusterSizeSummaryIB[iLayer][iStave][iChip], "Cluster size (Pixel)", "Counts", 1, 1.10);

          hClusterTopologySummaryIB[iLayer][iStave][iChip] = new TH1D(Form("Layer%d/Stave%d/CHIP%d/ClusterTopology", iLayer, iStave, iChip), Form("Layer%dStave%dCHIP%dClusterTopology", iLayer, iStave, iChip), 300, 0, 300);
          hClusterTopologySummaryIB[iLayer][iStave][iChip]->SetTitle(Form("Cluster Topology on Layer %d Stave %d Chip %d", iLayer, iStave, iChip));
          addObject(hClusterTopologySummaryIB[iLayer][iStave][iChip]);
          formatAxes(hClusterTopologySummaryIB[iLayer][iStave][iChip], "Cluster topology (ID)", "Counts", 1, 1.10);

          hAverageClusterSizeSummaryIB[iLayer]->GetXaxis()->SetBinLabel(iChip + 1, Form("Chip %i", iChip));
          hAverageClusterSizeMonitorIB[iLayer]->GetXaxis()->SetBinLabel(iChip + 1, Form("Chip %i", iChip));
          hAverageClusterOccupancySummaryIB[iLayer]->GetXaxis()->SetBinLabel(iChip + 1, Form("Chip %i", iChip));
          hAverageClusterOccupancyMonitorIB[iLayer]->GetXaxis()->SetBinLabel(iChip + 1, Form("Chip %i", iChip));
        }
      }
    } else {

      hAverageClusterOccupancySummaryOB[iLayer] = new TH2D(Form("Layer%d/ClusterOccupation", iLayer), Form("Layer%dClusterOccupancy", iLayer), mNLanePerHic[iLayer] * mNHicPerStave[iLayer], 0, mNLanePerHic[iLayer] * mNHicPerStave[iLayer], mNStaves[iLayer], 0, mNStaves[iLayer]);
      hAverageClusterOccupancySummaryOB[iLayer]->SetTitle(Form("Cluster Occupancy on Layer %d", iLayer));
      addObject(hAverageClusterOccupancySummaryOB[iLayer]);
      formatAxes(hAverageClusterOccupancySummaryOB[iLayer], "", "Stave Number", 1, 1.10);
      hAverageClusterOccupancySummaryOB[iLayer]->SetStats(0);
      hAverageClusterOccupancySummaryOB[iLayer]->SetBit(TH1::kIsAverage);
      hAverageClusterOccupancySummaryOB[iLayer]->GetYaxis()->SetLabelSize(0.02);
      hAverageClusterOccupancySummaryOB[iLayer]->GetXaxis()->SetLabelSize(0.02);
      hAverageClusterOccupancySummaryOB[iLayer]->GetXaxis()->SetTitleOffset(1.2);

      hAverageClusterOccupancyMonitorOB[iLayer] = new TH2D(Form("Layer%d/ClusterOccupationForLastNROFS", iLayer), Form("Layer%dClusterOccupancyForLastNROFS", iLayer), mNLanePerHic[iLayer] * mNHicPerStave[iLayer], 0, mNLanePerHic[iLayer] * mNHicPerStave[iLayer], mNStaves[iLayer], 0, mNStaves[iLayer]);
      hAverageClusterOccupancyMonitorOB[iLayer]->SetTitle(Form("Cluster Occupancy in last NROFS on Layer %d", iLayer));
      addObject(hAverageClusterOccupancyMonitorOB[iLayer]);
      formatAxes(hAverageClusterOccupancyMonitorOB[iLayer], "", "Stave Number", 1, 1.10);
      hAverageClusterOccupancyMonitorOB[iLayer]->SetStats(0);
      hAverageClusterOccupancyMonitorOB[iLayer]->SetBit(TH1::kIsAverage);
      hAverageClusterOccupancyMonitorOB[iLayer]->GetYaxis()->SetLabelSize(0.02);
      hAverageClusterOccupancyMonitorOB[iLayer]->GetXaxis()->SetLabelSize(0.02);
      hAverageClusterOccupancyMonitorOB[iLayer]->GetXaxis()->SetTitleOffset(1.2);

      hAverageClusterSizeSummaryOB[iLayer] = new TH2D(Form("Layer%d/AverageClusterSize", iLayer), Form("Layer%dAverageClusterSize", iLayer), mNLanePerHic[iLayer] * mNHicPerStave[iLayer], 0, mNLanePerHic[iLayer] * mNHicPerStave[iLayer], mNStaves[iLayer], 0, mNStaves[iLayer]);
      hAverageClusterSizeSummaryOB[iLayer]->SetTitle(Form("Average Cluster Size  on Layer %d", iLayer));
      addObject(hAverageClusterSizeSummaryOB[iLayer]);
      formatAxes(hAverageClusterSizeSummaryOB[iLayer], "", "Stave Number", 1, 1.10);
      hAverageClusterSizeSummaryOB[iLayer]->SetStats(0);
      hAverageClusterSizeSummaryOB[iLayer]->SetOption("colz");
      hAverageClusterSizeSummaryOB[iLayer]->SetBit(TH1::kIsAverage);
      hAverageClusterSizeSummaryOB[iLayer]->GetYaxis()->SetLabelSize(0.02);
      hAverageClusterSizeSummaryOB[iLayer]->GetXaxis()->SetLabelSize(0.02);
      hAverageClusterSizeSummaryOB[iLayer]->GetXaxis()->SetTitleOffset(1.2);

      hAverageClusterSizeMonitorOB[iLayer] = new TH2D(Form("Layer%d/AverageClusterSizeForLastNROFS", iLayer), Form("Layer%dAverageClusterSizeForLastNROFS", iLayer), mNLanePerHic[iLayer] * mNHicPerStave[iLayer], 0, mNLanePerHic[iLayer] * mNHicPerStave[iLayer], mNStaves[iLayer], 0, mNStaves[iLayer]);
      hAverageClusterSizeMonitorOB[iLayer]->SetTitle(Form("Average Cluster Size for the last NROFs on Layer %d", iLayer));
      addObject(hAverageClusterSizeMonitorOB[iLayer]);
      formatAxes(hAverageClusterSizeMonitorOB[iLayer], "", "Stave Number", 1, 1.10);
      hAverageClusterSizeMonitorOB[iLayer]->SetStats(0);
      hAverageClusterSizeMonitorOB[iLayer]->SetOption("colz");
      hAverageClusterSizeMonitorOB[iLayer]->SetBit(TH1::kIsAverage);
      hAverageClusterSizeMonitorOB[iLayer]->GetYaxis()->SetLabelSize(0.02);
      hAverageClusterSizeMonitorOB[iLayer]->GetXaxis()->SetLabelSize(0.02);
      hAverageClusterSizeMonitorOB[iLayer]->GetXaxis()->SetTitleOffset(1.2);

      for (Int_t iStave = 0; iStave < mNStaves[iLayer]; iStave++) {

        hClusterSizeSummaryOB[iLayer][iStave] = new TH1D(Form("Layer%d/Stave%d/ClusterSize", iLayer, iStave), Form("Layer%dStave%dClusterSize", iLayer, iStave), 100, 0, 100);
        hClusterSizeSummaryOB[iLayer][iStave]->SetTitle(Form("Cluster Size summary for Layer %d Stave %d", iLayer, iStave));
        addObject(hClusterSizeSummaryOB[iLayer][iStave]);
        formatAxes(hClusterSizeSummaryOB[iLayer][iStave], "Cluster size (Pixel)", "Counts", 1, 1.10);

        hGroupedClusterSizeSummaryOB[iLayer][iStave] = new TH1D(Form("Layer%d/Stave%d/GroupedClusterSize", iLayer, iStave), Form("Layer%dStave%dGroupedClusterSize", iLayer, iStave), 100, 0, 100);
        hGroupedClusterSizeSummaryOB[iLayer][iStave]->SetTitle(Form("Grouped Cluster Size summary for Layer %d Stave %d", iLayer, iStave));
        addObject(hGroupedClusterSizeSummaryOB[iLayer][iStave]);
        formatAxes(hGroupedClusterSizeSummaryOB[iLayer][iStave], "Cluster size (Pixel)", "Counts", 1, 1.10);

        hClusterTopologySummaryOB[iLayer][iStave] = new TH1D(Form("Layer%d/Stave%d/ClusterTopology", iLayer, iStave), Form("Layer%dStave%dClusterTopology", iLayer, iStave), 300, 0, 300);
        hClusterTopologySummaryOB[iLayer][iStave]->SetTitle(Form("Cluster Toplogy summary for Layer %d Stave %d", iLayer, iStave));
        addObject(hClusterTopologySummaryOB[iLayer][iStave]);
        formatAxes(hClusterTopologySummaryOB[iLayer][iStave], "Cluster toplogy (ID)", "Counts", 1, 1.10);

        hAverageClusterSizeSummaryOB[iLayer]->GetYaxis()->SetBinLabel(iStave + 1, Form("Stave %i", iStave));
        hAverageClusterSizeMonitorOB[iLayer]->GetYaxis()->SetBinLabel(iStave + 1, Form("Stave %i", iStave));
        hAverageClusterOccupancySummaryOB[iLayer]->GetYaxis()->SetBinLabel(iStave + 1, Form("Stave %i", iStave));
        hAverageClusterOccupancyMonitorOB[iLayer]->GetYaxis()->SetBinLabel(iStave + 1, Form("Stave %i", iStave));
        for (Int_t iLane = 0; iLane < mNLanePerHic[iLayer] * mNHicPerStave[iLayer]; iLane++) { // are used in TH2 construction, no need to keep on ccdb
          (iLayer < 5) ? (xLabel = Form("%s", OBLabel34[iLane])) : (xLabel = Form("%s", OBLabel56[iLane]));
          hAverageClusterSizeSummaryOB[iLayer]->GetXaxis()->SetBinLabel(iLane + 1, xLabel);
          hAverageClusterSizeMonitorOB[iLayer]->GetXaxis()->SetBinLabel(iLane + 1, xLabel);
          hAverageClusterOccupancySummaryOB[iLayer]->GetXaxis()->SetBinLabel(iLane + 1, xLabel);
          hAverageClusterOccupancyMonitorOB[iLayer]->GetXaxis()->SetBinLabel(iLane + 1, xLabel);
        }
      }
    }
  }
}

void ITSClusterTask::getJsonParameters()
{
  mNThreads = stoi(mCustomParameters.find("nThreads")->second);
  nBCbins = stoi(mCustomParameters.find("nBCbins")->second);
  mGeomPath = mCustomParameters["geomPath"];

  for (int ilayer = 0; ilayer < NLayer; ilayer++) {

    if (mCustomParameters["layer"][ilayer] != '0') {
      mEnableLayers[ilayer] = 1;
      ILOG(Info, Support) << "enable layer : " << ilayer << ENDM;
    } else {
      mEnableLayers[ilayer] = 0;
    }
  }
}

void ITSClusterTask::addObject(TObject* aObject)
{
  if (!aObject) {
    ILOG(Info, Support) << " ERROR: trying to add non-existent object " << ENDM;
    return;
  } else
    mPublishedObjects.push_back(aObject);
}

void ITSClusterTask::formatAxes(TH1* h, const char* xTitle, const char* yTitle, float xOffset, float yOffset)
{
  h->GetXaxis()->SetTitle(xTitle);
  h->GetYaxis()->SetTitle(yTitle);
  h->GetXaxis()->SetTitleOffset(xOffset);
  h->GetYaxis()->SetTitleOffset(yOffset);
}

void ITSClusterTask::publishHistos()
{
  for (unsigned int iObj = 0; iObj < mPublishedObjects.size(); iObj++) {
    getObjectsManager()->startPublishing(mPublishedObjects.at(iObj));
  }
}

} // namespace o2::quality_control_modules::its
