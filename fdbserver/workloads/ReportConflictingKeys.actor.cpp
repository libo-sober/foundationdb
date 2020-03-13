/*
 * ReportConflictingKeys.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/ReadYourWrites.h"
#include "fdbclient/SystemData.h"
#include "fdbserver/TesterInterface.actor.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "fdbserver/workloads/BulkSetup.actor.h"
#include "flow/actorcompiler.h" // This must be the last #include.

// For this test to report properly buggify must be disabled (flow.h) , and failConnection must be disabled in
// (sim2.actor.cpp)
struct ReportConflictingKeysWorkload : TestWorkload {

	double testDuration, transactionsPerSecond, addReadConflictRangeProb, addWriteConflictRangeProb;
	Key keyPrefix;

	int nodeCount, actorCount, keyBytes, valueBytes, readConflictRangeCount, writeConflictRangeCount;

	PerfIntCounter invalidReports, commits, conflicts, retries, xacts;

	ReportConflictingKeysWorkload(WorkloadContext const& wcx)
	  : TestWorkload(wcx), invalidReports("InvalidReports"), conflicts("Conflicts"), retries("Retries"),
	    commits("Commits"), xacts("Transactions") {
		testDuration = getOption(options, LiteralStringRef("testDuration"), 10.0);
		// transactionsPerSecond = getOption(options, LiteralStringRef("transactionsPerSecond"), 5000.0) / clientCount;
		actorCount = getOption(options, LiteralStringRef("actorsPerClient"), 1);
		keyPrefix = unprintable(
		    getOption(options, LiteralStringRef("keyPrefix"), LiteralStringRef("ReportConflictingKeysWorkload"))
		        .toString());
		keyBytes = getOption(options, LiteralStringRef("keyBytes"), 16);

		readConflictRangeCount = getOption(options, LiteralStringRef("readConflictRangeCountPerTx"), 1);
		writeConflictRangeCount = getOption(options, LiteralStringRef("writeConflictRangeCountPerTx"), 1);
		ASSERT(readConflictRangeCount >= 1 && writeConflictRangeCount >= 1);
		// modeled by geometric distribution: (1 - prob) / prob = mean - 1, where we add at least one conflictRange to
		// each tx
		addReadConflictRangeProb = (readConflictRangeCount - 1.0) / readConflictRangeCount;
		addWriteConflictRangeProb = (writeConflictRangeCount - 1.0) / writeConflictRangeCount;
		ASSERT(keyPrefix.size() + 16 <= keyBytes); // make sure the string format is valid
		nodeCount = getOption(options, LiteralStringRef("nodeCount"), 100);
	}

	std::string description() override { return "ReportConflictingKeysWorkload"; }

	Future<Void> setup(Database const& cx) override { return Void(); }

	Future<Void> start(const Database& cx) override { return _start(cx->clone(), this); }

	ACTOR Future<Void> _start(Database cx, ReportConflictingKeysWorkload* self) {
		if (self->clientId == 0) wait(timeout(self->conflictingClient(cx, self), self->testDuration, Void()));
		return Void();
	}

	Future<bool> check(Database const& cx) override { return invalidReports.getValue() == 0; }

	void getMetrics(vector<PerfMetric>& m) override {
		m.push_back(PerfMetric("Measured Duration", testDuration, true));
		m.push_back(xacts.getMetric());
		m.push_back(PerfMetric("Transactions/sec", xacts.getValue() / testDuration, true));
		m.push_back(commits.getMetric());
		m.push_back(PerfMetric("Commits/sec", commits.getValue() / testDuration, true));
		m.push_back(conflicts.getMetric());
		m.push_back(PerfMetric("Conflicts/sec", conflicts.getValue() / testDuration, true));
		m.push_back(retries.getMetric());
		m.push_back(PerfMetric("Retries/sec", retries.getValue() / testDuration, true));
	}

	// disable the default timeout setting
	double getCheckTimeout() override { return std::numeric_limits<double>::max(); }

	// Copied from tester.actor.cpp, added parameter to determine the key's length
	Key keyForIndex(int n) {
		double p = (double)n / nodeCount;
		int paddingLen = keyBytes - 16 - keyPrefix.size();
		// left padding by zero
		return StringRef(format("%0*llx", paddingLen, *(uint64_t*)&p)).withPrefix(keyPrefix);
	}

	void addRandomReadConflictRange(ReadYourWritesTransaction* tr, std::vector<KeyRange>* readConflictRanges) {
		int startIdx, endIdx;
		Key startKey, endKey;
		do { // add at least one
			startIdx = deterministicRandom()->randomInt(0, nodeCount);
			endIdx = deterministicRandom()->randomInt(startIdx, nodeCount + 1);
			startKey = keyForIndex(startIdx);
			endKey = keyForIndex(endIdx);
			tr->addReadConflictRange(KeyRangeRef(startKey, endKey));
			if (readConflictRanges) readConflictRanges->push_back(KeyRangeRef(startKey, endKey));
		} while (deterministicRandom()->random01() < addReadConflictRangeProb);
	}

	void addRandomWriteConflictRange(ReadYourWritesTransaction* tr, std::vector<KeyRange>* writeConflictRanges) {
		int startIdx, endIdx;
		Key startKey, endKey;
		do { // add at least one
			startIdx = deterministicRandom()->randomInt(0, nodeCount);
			endIdx = deterministicRandom()->randomInt(startIdx, nodeCount + 1);
			startKey = keyForIndex(startIdx);
			endKey = keyForIndex(endIdx);
			tr->addWriteConflictRange(KeyRangeRef(startKey, endKey));
			if (writeConflictRanges) writeConflictRanges->push_back(KeyRangeRef(startKey, endKey));
		} while (deterministicRandom()->random01() < addWriteConflictRangeProb);
	}

	ACTOR Future<Void> conflictingClient(Database cx, ReportConflictingKeysWorkload* self) {

		state ReadYourWritesTransaction tr1(cx);
		state ReadYourWritesTransaction tr2(cx);
		state std::vector<KeyRange> readConflictRanges;
		state std::vector<KeyRange> writeConflictRanges;

		loop {
			try {
				tr2.setOption(FDBTransactionOptions::REPORT_CONFLICTING_KEYS);
				// If READ_YOUR_WRITES_DISABLE set, it behaves like native transaction object
				// where overlapped conflict ranges are not merged.
				if (deterministicRandom()->random01() < 0.5)
					tr1.setOption(FDBTransactionOptions::READ_YOUR_WRITES_DISABLE);
				if (deterministicRandom()->random01() < 0.5)
					tr2.setOption(FDBTransactionOptions::READ_YOUR_WRITES_DISABLE);
				// We have the two tx with same grv, then commit the first
				// If the second one is not able to commit due to conflicts, verify the returned conflicting keys
				// Otherwise, there is no conflicts between tr1's writeConflictRange and tr2's readConflictRange
				Version readVersion = wait(tr1.getReadVersion());
				tr2.setVersion(readVersion);
				self->addRandomReadConflictRange(&tr1, nullptr);
				self->addRandomWriteConflictRange(&tr1, &writeConflictRanges);
				++self->commits;
				wait(tr1.commit());
				++self->xacts;

				state bool foundConflict = false;
				try {
					self->addRandomReadConflictRange(&tr2, &readConflictRanges);
					self->addRandomWriteConflictRange(&tr2, nullptr);
					++self->commits;
					wait(tr2.commit());
					++self->xacts;
				} catch (Error& e) {
					if (e.code() != error_code_not_committed) throw e;
					foundConflict = true;
					++self->conflicts;
				}
				// check API correctness
				if (foundConflict) {
					// \xff\xff/transaction/conflicting_keys is always initialized to false, skip it here
					state KeyRange ckr =
					    KeyRangeRef(keyAfter(LiteralStringRef("").withPrefix(conflictingKeysAbsolutePrefix)),
					                LiteralStringRef("\xff\xff").withPrefix(conflictingKeysAbsolutePrefix));
					// The getRange here using the special key prefix "\xff\xff/transaction/conflicting_keys/" happens
					// locally Thus, the error handling is not needed here
					Future<Standalone<RangeResultRef>> conflictingKeyRangesFuture =
					    tr2.getRange(ckr, readConflictRanges.size() * 2);
					ASSERT(conflictingKeyRangesFuture.isReady());
					const Standalone<RangeResultRef> conflictingKeyRanges = conflictingKeyRangesFuture.get();
					ASSERT(conflictingKeyRanges.size() && (conflictingKeyRanges.size() % 2 == 0));
					for (int i = 0; i < conflictingKeyRanges.size(); i += 2) {
						KeyValueRef startKeyWithPrefix = conflictingKeyRanges[i];
						ASSERT(startKeyWithPrefix.value == conflictingKeysTrue);
						KeyValueRef endKeyWithPrefix = conflictingKeyRanges[i + 1];
						ASSERT(endKeyWithPrefix.value == conflictingKeysFalse);
						// Remove the prefix of returning keys
						Key startKey = startKeyWithPrefix.key.removePrefix(conflictingKeysAbsolutePrefix);
						Key endKey = endKeyWithPrefix.key.removePrefix(conflictingKeysAbsolutePrefix);
						KeyRangeRef kr = KeyRangeRef(startKey, endKey);
						if (!std::any_of(readConflictRanges.begin(), readConflictRanges.end(), [&kr](KeyRange rCR) {
							    // Read_conflict_range remains same in the resolver.
							    // Thus, the returned keyrange is either the original read_conflict_range or merged
							    // by several overlapped ones in either cases, it contains at least one original
							    // read_conflict_range
							    return kr.contains(rCR);
						    })) {
							++self->invalidReports;
							TraceEvent(SevError, "TestFailure")
							    .detail("Reason",
							            "Returned conflicting keys are not original or merged readConflictRanges");
						} else if (!std::any_of(writeConflictRanges.begin(), writeConflictRanges.end(),
						                        [&kr](KeyRange wCR) {
							                        // Returned key range should be conflicting with at least one
							                        // writeConflictRange
							                        return kr.intersects(wCR);
						                        })) {
							++self->invalidReports;
							TraceEvent(SevError, "TestFailure")
							    .detail("Reason", "Returned keyrange is not conflicting with any writeConflictRange");
						}
					}
				} else {
					// make sure no conflicts between tr2's readConflictRange and tr1's writeConflictRange
					for (const KeyRange& rCR : readConflictRanges) {
						if (std::any_of(writeConflictRanges.begin(), writeConflictRanges.end(), [&rCR](KeyRange wCR) {
							    bool result = wCR.intersects(rCR);
							    if (result)
								    TraceEvent(SevError, "TestFailure")
								        .detail("WriteConflictRange", wCR.toString())
								        .detail("ReadConflictRange", rCR.toString());
							    return result;
						    })) {
							++self->invalidReports;
							TraceEvent(SevError, "TestFailure").detail("Reason", "No conflicts returned but it should");
							break;
						}
					}
				}
			} catch (Error& e) {
				state Error e2 = e;
				wait(tr1.onError(e2));
				wait(tr2.onError(e2));
			}
			readConflictRanges.clear();
			writeConflictRanges.clear();
			tr1.reset();
			tr2.reset();
		}
	}
};

WorkloadFactory<ReportConflictingKeysWorkload> ReportConflictingKeysWorkload("ReportConflictingKeys");
