//--------------------------------------------------------------------------//
/// Copyright 2026 Milos Tosic. All Rights Reserved.                       ///
/// License: http://www.opensource.org/licenses/BSD-2-Clause               ///
//--------------------------------------------------------------------------//

#ifndef RTM_MTUNER_INSIGHTS_H
#define RTM_MTUNER_INSIGHTS_H

#include <QtCore/QString>
#include <stdint.h>
#include <vector>

namespace rtm { class Capture; struct StackTrace; }

//--------------------------------------------------------------------------
/// A single rule-based finding about a capture (deterministic, explainable).
//--------------------------------------------------------------------------
struct Insight
{
	enum Severity { Info = 0, Low, Medium, High };

	Severity			m_severity   = Info;
	QString				m_category;					///< "Leaks", "Churn", "Peak", "Overhead", "Growth"
	QString				m_title;					///< one-line headline
	QString				m_detail;					///< explanation with concrete numbers
	rtm::StackTrace*	m_stackTrace = nullptr;		///< representative call site for deep-linking (or null)
	uint64_t			m_time       = 0;			///< representative time (for graph highlight)
	bool				m_hasTime    = false;
};

/// Runs the rule-based analyzers over a loaded capture and returns insights in priority order.
/// Pure analysis over already-computed aggregates - no ML, no heavy work.
std::vector<Insight> captureInsightsAnalyze(rtm::Capture* _capture);

#endif // RTM_MTUNER_INSIGHTS_H
