#ifndef _ots_ConsoleSupervisor_h_
#define _ots_ConsoleSupervisor_h_

#include <boost/regex.hpp>
#include <boost/tokenizer.hpp>
#include "otsdaq/CoreSupervisors/CoreSupervisorBase.h"

#include <mutex>  //for std::mutex
#include <queue>  //for std::queue

namespace ots
{

// clang-format off

////// class define
/// ConsoleSupervisor
///	This class handles the presentation of Message Facility printouts to the web desktop
/// 	Console.
class ConsoleSupervisor : public CoreSupervisorBase
{
  public:
	XDAQ_INSTANTIATOR();

	struct CustomTriggeredAction_t
	{
		std::vector<std::string> 	needleSubstrings; /* custom trigger needle substrings */
		std::string 				action; /* action */
		size_t						triggeredMessageCountIndex = -1; /* message arrival count that fired the trigger */
		size_t						occurrences = 0;
		size_t					    triggerOnCount = 1; /* number of times event occurs before triggering action */
		size_t 						doLoop = 0; /* Wether to keep triggering on further occurrences */

	}; //end CustomTriggeredAction_t struct


								ConsoleSupervisor					(xdaq::ApplicationStub* s);
	virtual 					~ConsoleSupervisor					(void);

	void 						init								(void);
	void 						destroy								(void);

	xoap::MessageReference 		resetConsoleCounts					(xoap::MessageReference message);

	virtual void				defaultPage							(xgi::Input* in, xgi::Output* out) override;
	virtual void 				request								(const std::string&               requestType,
																	cgicc::Cgicc&                    cgiIn,
																	HttpXmlDocument&                 xmlOut,
																	const WebUsers::RequestUserInfo& userInfo) override;
	virtual std::string 		getStatusProgressDetail				(void) override;

	virtual void 				forceSupervisorPropertyValues		(void) override;  ///< override to force
																		///< supervisor property
																		///< values (and ignore user
																		///< settings)

	void 						doTriggeredAction					(const CustomTriggeredAction_t& triggeredAction);

	std::atomic<bool>						customTriggerActionThreadExists_ = false;
	static const std::set<std::string> 		CUSTOM_TRIGGER_ACTIONS;

  private:
	static void 				messageFacilityReceiverWorkLoop		(ConsoleSupervisor* cs);
	static void 				customTriggerActionThread			(ConsoleSupervisor* cs);
	void 						insertMessageRefresh				(HttpXmlDocument* xmldoc, const size_t lastUpdateCount);
	void 						prependHistoricMessages				(HttpXmlDocument* xmlOut, const size_t earliestOnhandMessageCount);

	void 						addCustomTriggeredAction			(const std::string& triggerNeedle,
																	 const std::string& triggerAction,
																	 uint32_t priority = -1,
																	 uint32_t triggerOnCount = 1,
																	 bool doLoop = false);
	uint32_t					modifyCustomTriggeredAction			(const std::string& currentNeedle,
																	 const std::string& modifyType,
																	 const std::string& setNeedle,
																	 const std::string& setAction,
																	 uint32_t setPriority,
																	 uint32_t setTriggerOnCount,
																	 bool setDoLoop);

	void						loadCustomCountList					(void);
	void						saveCustomCountList					(void);

  public:
	/// UDP Message Format:
	/// UDPMFMESSAGE|TIMESTAMP|SEQNUM|HOSTNAME|HOSTADDR|SEVERITY|CATEGORY|APPLICATION|PID|ITERATION|MODULE|(FILE|LINE)|MESSAGE
	/// FILE and LINE are only printed for s67+
	struct ConsoleMessageStruct
	{
		ConsoleMessageStruct(const std::string& msg, const size_t count,
			std::vector<CustomTriggeredAction_t>& priorityCustomTriggerList)
			: countStamp(count)
		{
			boost::regex timestamp_regex_("(\\d{2}-[^-]*-\\d{4}\\s\\d{2}:\\d{2}:\\d{2})");
			boost::regex file_line_regex_("^\\s*([^:]*\\.[^:]{1,3}):(\\d+)(.*)");

			boost::char_separator<char> sep("|", "", boost::keep_empty_tokens);
			typedef boost::tokenizer<boost::char_separator<char>> tokenizer;
			tokenizer                                             tokens(msg, sep);
			tokenizer::iterator                                   it = tokens.begin();

			// There may be syslog garbage in the first token before the timestamp...
			boost::smatch res;
			while(it != tokens.end() && !boost::regex_search(*it, res, timestamp_regex_))
			{
				++it;
			}

			struct tm   tm;
			std::string value(res[1].first, res[1].second);
			strptime(value.c_str(), "%d-%b-%Y %H:%M:%S", &tm);
			tm.tm_isdst = -1;
			fields[FieldType::TIMESTAMP] = std::to_string(mktime(&tm));

			auto prevIt = it;
			try
			{
				if(it != tokens.end() && ++it != tokens.end() /* Advances it */)
				{
					// seqNum = std::stoi(*it);
					fields[FieldType::SEQID] = *it;
				}
			}
			catch(const std::invalid_argument& e)
			{
				it = prevIt;
			}
			if(it != tokens.end() && ++it != tokens.end() /* Advances it */)
			{
				// hostname = *it;
				fields[FieldType::HOSTNAME] = *it;
			}
			if(it != tokens.end() && ++it != tokens.end() /* Advances it */)
			{
				; //not needed (yet): hostaddr = *it;
			}
			if(it != tokens.end() && ++it != tokens.end() /* Advances it */)
			{
				// sev = mf::ELseverityLevel(*it);
				fields[FieldType::LEVEL] = mf::ELseverityLevel(*it).getName();
			}
			if(it != tokens.end() && ++it != tokens.end() /* Advances it */)
			{
				// category = *it;
				fields[FieldType::LABEL] = *it;
			}
			if(it != tokens.end() && ++it != tokens.end() /* Advances it */)
			{
				// application = *it;
				fields[FieldType::SOURCE] = *it;
			}
			prevIt = it;
			try
			{
				if(it != tokens.end() && ++it != tokens.end() /* Advances it */)
				{
					// pid = std::stol(*it);
					fields[FieldType::SOURCEID] = *it;
				}
			}
			catch(const std::invalid_argument& e)
			{
				it = prevIt;
			}
			if(it != tokens.end() && ++it != tokens.end() /* Advances it */)
			{
				; //not needed (yet): eventID = *it;
			}
			if(it != tokens.end() && ++it != tokens.end() /* Advances it */)
			{
				; //not needed (yet): module = *it;
			}
			if(it != tokens.end() && ++it != tokens.end() /* Advances it */)
			{
				// file = *it;
				fields[FieldType::FILE] = *it;
			}
			if(it != tokens.end() && ++it != tokens.end() /* Advances it */)
			{
				// line = *it;
				fields[FieldType::LINE] = *it;
			}
			std::ostringstream oss;
			bool               first = true;
			while(it != tokens.end() && ++it != tokens.end() /* Advances it */)
			{
				if(!first)
				{
					oss << "|";
				}
				else
				{
					first = false;
				}
				oss << *it;
			}
			fields[FieldType::MSG] = oss.str();

#if 0
			for (auto& field : fields) {
				std::cout << "Field " << field.second.fieldName << ": " << field.second.fieldValue
						  << std::endl;
			}
#endif

			//check custom triggers
			//Note: to avoid recursive triggers, Label=Console can not trigger
			for(auto& triggeredAction : priorityCustomTriggerList)
			{
				if(getLabel() == "Console") break; ///<Note: to avoid recursive triggers, Label=Console can not trigger

				size_t pos = 0;
				bool foundAll = false;
				for(const auto& needleSubstring : triggeredAction.needleSubstrings)
					if((pos = getMsg().find(needleSubstring)) == std::string::npos)
					{
						foundAll = false;
						//FOR DEBUGGING std::cout << "Not a full match on '" << needleSubstring << ".' Message: " <<
						// 	getSourceIDAsNumber() << ":" << getMsg().substr(0,100) << __E__;
						break; ///<not a full match
					}
					else
						foundAll = true; ///<still possible

				if(foundAll) ///<trigger fired so copy triggeredAction and tag with message sequence ID
				{
					//FOR DEBUGGING std::cout << "Full match of custom trigger! Message: " <<
					// 	getSourceIDAsNumber() << ":" << getMsg().substr(0,100) << __E__;
					triggeredAction.occurrences++; ///<increment occurrences
					customTriggerMatch = triggeredAction;
					customTriggerMatch.triggeredMessageCountIndex = getCount();
					break;
				}
			} //end custom trigger search

		} //end ConsoleMessageStruct constructor

		void setCustomTriggerMatch(const CustomTriggeredAction_t& forcedCustomTriggerMatch) { customTriggerMatch = forcedCustomTriggerMatch; }
		const CustomTriggeredAction_t& getCustomTriggerMatch() const { return customTriggerMatch; }
		bool 		hasCustomTriggerMatchAction() const { return customTriggerMatch.action.size(); }
		const std::string& getTime() const { return fields.at(FieldType::TIMESTAMP); }
		void 			   setTime(time_t t) { fields[FieldType::TIMESTAMP] = std::to_string(t); }
		const std::string& getMsg() const { return fields.at(FieldType::MSG); }
		const std::string& getLabel() const { return fields.at(FieldType::LABEL); }
		const std::string& getLevel() const {
			//identify 9+ levels as TRACE (mf calls them DEBUG)
			if(getMsg().size() > 4)
			{
				if(getMsg()[0] == '9' && getMsg()[1] == ':') return ConsoleMessageStruct::LABEL_TRACE;
				if(getMsg()[0] >= '1' && getMsg()[0] <= '3' &&
					getMsg()[1] >= '0' && getMsg()[1] <= '9' &&
					getMsg()[2] == ':') return ConsoleMessageStruct::LABEL_TRACE_PLUS;
			}
			return fields.at(FieldType::LEVEL);
		}
		const std::string& getFile() const { return fields.at(FieldType::FILE); }
		const std::string& getLine() const { return fields.at(FieldType::LINE); }

		const std::string& getSourceID() const { return fields.at(FieldType::SOURCEID); }
		uint32_t    getSourceIDAsNumber() const
		{
			auto val = fields.at(FieldType::SOURCEID);
			if(val != "")
			{
				return std::stoul(val);
			}
			return 0;
		}
		const std::string& getSource() const { return fields.at(FieldType::SOURCE); }
		const std::string& getSequenceID() const { return fields.at(FieldType::SEQID); }
		size_t      getSequenceIDAsNumber() const
		{
			auto val = fields.at(FieldType::SEQID);
			if(val != "")
			{
				return std::stoul(val);
			}
			return 0;
		}

		//count is incrementing number across all sources created at ConsoleSupervisor
		size_t getCount() const { return countStamp; }

		// define field index enum alias
		enum class FieldType
		{  // must be in order of appearance in buffer
			TIMESTAMP,
			//count is incrementing number across all sources created at ConsoleSupervisor
			SEQID,  ///< sequence ID is incrementing number independent from each source
			LEVEL,  ///< aka SEVERITY
			LABEL,
			SOURCEID,
			HOSTNAME,
			SOURCE,
			FILE,
			LINE,
			MSG,
		};

		mutable std::unordered_map<FieldType, std::string /* fieldValue */> fields;
		static const std::map<FieldType, std::string /* fieldNames */> fieldNames;

	  private:
		size_t countStamp; ///<count is incrementing number across all sources created at ConsoleSupervisor
		CustomTriggeredAction_t customTriggerMatch;

		static const std::string LABEL_TRACE, LABEL_TRACE_PLUS;
	}; //end ConsoleMessageStruct

  private:
	void 				addMessageToResponse				(HttpXmlDocument* xmlOut, ConsoleSupervisor::ConsoleMessageStruct& msg);

	std::deque<ConsoleMessageStruct> messages_;
	std::mutex                       messageMutex_;
	size_t messageCount_;  ///<"unique" incrementing ID for messages
	size_t maxMessageCount_, maxClientMessageRequest_;

	std::map<std::string /*TRACE hostname*/, std::string /*xdaq context hostname*/>
											traceMapToXDAQHostname_;

	/// members for the refresh handler, ConsoleSupervisor::insertMessageRefresh
	xercesc::DOMElement* 					refreshParent_;

	std::vector<CustomTriggeredAction_t>  		priorityCustomTriggerList_;
	std::queue<CustomTriggeredAction_t>	 		customTriggerActionQueue_;

	///for system status:
	size_t 			errorCount_ = 0, warnCount_ = 0, infoCount_ = 0;
	std::string 	lastErrorMessage_, lastWarnMessage_, lastInfoMessage_, firstErrorMessage_, firstWarnMessage_, firstInfoMessage_;
	time_t			lastErrorMessageTime_ = 0, lastWarnMessageTime_ = 0, lastInfoMessageTime_ = 0, firstErrorMessageTime_ = 0, firstWarnMessageTime_ = 0, firstInfoMessageTime_ = 0;
};

// clang-format on
}  // namespace ots

#endif
