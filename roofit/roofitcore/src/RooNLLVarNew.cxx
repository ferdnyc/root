/*
 * Project: RooFit
 * Authors:
 *   Jonas Rembser, CERN 2021
 *   Emmanouil Michalainas, CERN 2021
 *
 * Copyright (c) 2021, CERN
 *
 * Redistribution and use in source and binary forms,
 * with or without modification, are permitted according to the terms
 * listed in LICENSE (http://roofit.sourceforge.net/license.txt)
 */

/**
\file RooNLLVarNew.cxx
\class RooNLLVarNew
\ingroup Roofitcore

This is a simple class designed to produce the nll values needed by the fitter.
In contrast to the `RooNLLVar` class, any logic except the bare minimum has been
transfered away to other classes, like the `RooFitDriver`. This class also calls
functions from `RooBatchCompute` library to provide faster computation times.
**/

#include <RooNLLVarNew.h>

#include <RooAddition.h>
#include <RooFormulaVar.h>
#include <RooNaNPacker.h>
#include <RooFit/Detail/Buffers.h>

#include <ROOT/StringUtils.hxx>

#include <Math/Util.h>
#include <TMath.h>

#include <numeric>
#include <stdexcept>
#include <vector>

using namespace ROOT::Experimental;

// Declare constexpr static members to make them available if odr-used in C++14.
constexpr const char *RooNLLVarNew::weightVarName;
constexpr const char *RooNLLVarNew::weightVarNameSumW2;

namespace {

std::unique_ptr<RooAbsReal>
createFractionInRange(RooAbsPdf const &pdf, RooArgSet const &observables, std::string const &rangeNames)
{

   RooArgSet observablesInPdf;
   pdf.getObservables(&observables, observablesInPdf);

   return std::unique_ptr<RooAbsReal>{
      pdf.createIntegral(observablesInPdf, &observablesInPdf, pdf.getIntegratorConfig(), rangeNames.c_str())};
}

template <class Input>
double kahanSum(Input const &input)
{
   return ROOT::Math::KahanSum<double, 4u>::Accumulate(input.begin(), input.end()).Sum();
}

} // namespace

/** Construct a RooNLLVarNew
\param name the name
\param title the title
\param pdf The pdf for which the nll is computed for
\param observables The observabes of the pdf
\param isExtended Set to true if this is an extended fit
\param rangeName the range name
**/
RooNLLVarNew::RooNLLVarNew(const char *name, const char *title, RooAbsPdf &pdf, RooArgSet const &observables,
                           bool isExtended, std::string const &rangeName)
   : RooAbsReal(name, title), _pdf{"pdf", "pdf", this, pdf}, _observables{observables}, _isExtended{isExtended}
{
   if (!rangeName.empty()) {
      auto term = createFractionInRange(pdf, observables, rangeName);
      _fractionInRange =
         std::make_unique<RooTemplateProxy<RooAbsReal>>("_fractionInRange", "_fractionInRange", this, *term);
      this->addOwnedComponents(std::move(term));
   }
}

RooNLLVarNew::RooNLLVarNew(const RooNLLVarNew &other, const char *name)
   : RooAbsReal(other, name), _pdf{"pdf", this, other._pdf}, _observables{other._observables}
{
   if (other._fractionInRange)
      _fractionInRange =
         std::make_unique<RooTemplateProxy<RooAbsReal>>("_fractionInRange", this, *other._fractionInRange);
}

/** Compute multiple negative logs of propabilities

\param output An array of doubles where the computation results will be stored
\param nOut not used
\note nEvents is the number of events to be processed (the dataMap size)
\param dataMap A map containing spans with the input data for the computation
**/
void RooNLLVarNew::computeBatch(cudaStream_t * /*stream*/, double *output, size_t /*nOut*/,
                                RooFit::Detail::DataMap const &dataMap) const
{
   std::size_t nEvents = dataMap.at(_pdf).size();
   auto probas = dataMap.at(_pdf);

   auto logProbasBuffer = ROOT::Experimental::Detail::makeCpuBuffer(nEvents);
   RooSpan<double> logProbas{logProbasBuffer->cpuWritePtr(), nEvents};
   (*_pdf).getLogProbabilities(probas, logProbas.data());

   auto &nameReg = RooNameReg::instance();
   auto weights = dataMap.at(nameReg.constPtr((_prefix + weightVarName).c_str()));
   auto weightsSumW2 = dataMap.at(nameReg.constPtr((_prefix + weightVarNameSumW2).c_str()));
   auto weightSpan = _weightSquared ? weightsSumW2 : weights;

   if ((_isExtended || _fractionInRange) && _sumWeight == 0.0) {
      _sumWeight = weights.size() == 1 ? weights[0] * nEvents : kahanSum(weights);
   }
   if ((_isExtended || _fractionInRange) && _weightSquared && _sumWeight2 == 0.0) {
      _sumWeight2 = weights.size() == 1 ? weightsSumW2[0] * nEvents : kahanSum(weightsSumW2);
   }
   double sumCorrectionTerm = 0;
   if (_fractionInRange) {
      auto fractionInRangeSpan = dataMap.at(*_fractionInRange);
      if (fractionInRangeSpan.size() == 1) {
         sumCorrectionTerm = (_weightSquared ? _sumWeight2 : _sumWeight) * std::log(fractionInRangeSpan[0]);
      } else {
         if (weightSpan.size() == 1) {
            double fractionInRangeLogSum = 0.0;
            for (std::size_t i = 0; i < fractionInRangeSpan.size(); ++i) {
               fractionInRangeLogSum += std::log(fractionInRangeSpan[i]);
            }
            sumCorrectionTerm = weightSpan[0] * fractionInRangeLogSum;
         } else {
            // We don't need to use the library for now because the weights and
            // correction term integrals are always in the CPU map.
            sumCorrectionTerm = 0.0;
            for (std::size_t i = 0; i < nEvents; ++i) {
               sumCorrectionTerm += weightSpan[i] * std::log(fractionInRangeSpan[i]);
            }
         }
      }
   }

   ROOT::Math::KahanSum<double> kahanProb;
   RooNaNPacker packedNaN(0.f);

   for (std::size_t i = 0; i < nEvents; ++i) {

      double eventWeight = weightSpan.size() > 1 ? weightSpan[i] : weightSpan[0];
      if (0. == eventWeight * eventWeight)
         continue;

      const double term = -eventWeight * logProbas[i];

      kahanProb.Add(term);
      packedNaN.accumulate(term);
   }

   if (packedNaN.getPayload() != 0.) {
      // Some events with evaluation errors. Return "badness" of errors.
      kahanProb = packedNaN.getNaNWithPayload();
   }

   if (_isExtended) {
      assert(_sumWeight != 0.0);
      double expected = _pdf->expectedEvents(&_observables);
      if (_fractionInRange) {
         expected *= dataMap.at(*_fractionInRange)[0];
      }
      kahanProb += _pdf->extendedTerm(_sumWeight, expected, _weightSquared ? _sumWeight2 : 0.0);
   }
   if (_fractionInRange) {
      kahanProb += sumCorrectionTerm;
   }
   output[0] = kahanProb.Sum();
}

double RooNLLVarNew::evaluate() const
{
   return _value;
}

void RooNLLVarNew::getParametersHook(const RooArgSet * /*nset*/, RooArgSet *params, bool /*stripDisconnected*/) const
{
   // strip away the observables and weights
   params->remove(_observables, true, true);
}

////////////////////////////////////////////////////////////////////////////////
/// Replaces all observables and the weight variable of this NLL with clones
/// that only differ by a prefix added to the names. Used for simultaneous fits.
/// \return A RooArgSet with the new observable args.
/// \param[in] prefix The prefix to add to the observables and weight names.
RooArgSet RooNLLVarNew::prefixObservableAndWeightNames(std::string const &prefix)
{
   _prefix = prefix;

   RooArgSet obsSet{_observables};
   RooArgSet obsClones;
   obsSet.snapshot(obsClones);
   for (RooAbsArg *arg : obsClones) {
      arg->setAttribute((std::string("ORIGNAME:") + arg->GetName()).c_str());
      arg->SetName((prefix + arg->GetName()).c_str());
   }
   recursiveRedirectServers(obsClones, false, true);

   RooArgSet newObservables{obsClones};

   setObservables(obsClones);
   addOwnedComponents(std::move(obsClones));

   return newObservables;
}

////////////////////////////////////////////////////////////////////////////////
/// Toggles the weight square correction.
void RooNLLVarNew::applyWeightSquared(bool flag)
{
   _weightSquared = flag;
}