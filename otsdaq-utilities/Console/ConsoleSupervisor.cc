#include "otsdaq-utilities/Console/ConsoleSupervisor.h"
#include <xdaq/NamespaceURI.h>
#include "otsdaq/CgiDataUtilities/CgiDataUtilities.h"
#include "otsdaq/Macros/CoutMacros.h"
#include "otsdaq/MessageFacility/MessageFacility.h"
#include "otsdaq/NetworkUtilities/ReceiverSocket.h"
#include "otsdaq/XmlUtilities/HttpXmlDocument.h"

#include <dirent.h>    //for DIR
#include <sys/stat.h>  //for mkdir
#include <fstream>
#include <iostream>
#include <string>
#include <thread>  //for std::thread

using namespace ots;

/// UDP Message Format:
/// UDPMESSAGE|TIMESTAMP|SEQNUM|HOSTNAME|HOSTADDR|SEVERITY|CATEGORY|APPLICATION|PID|ITERATION|MODULE|(FILE|LINE)|MESSAGE
/// FILE and LINE are only printed for s67+
///
XDAQ_INSTANTIATOR_IMPL(ConsoleSupervisor)

#define USER_CONSOLE_PREF_PATH \
	std::string(__ENV__("SERVICE_DATA_PATH")) + "/ConsolePreferences/"
#define USER_CONSOLE_SNAPSHOT_PATH \
	std::string(__ENV__("SERVICE_DATA_PATH")) + "/ConsoleSnapshots/"
#define USERS_PREFERENCES_FILETYPE "pref"
#define CUSTOM_COUNT_LIST_FILENAME std::string("CustomCountList.dat")

#define QUIET_CFG_FILE                    \
	std::string(__ENV__("USER_DATA")) +   \
	    "/MessageFacilityConfigurations/" \
	    "QuietForwarder.cfg"

#define CONSOLE_SPECIAL_ERROR                                                            \
	std::string("|30-Aug-2019 15:30:17 CDT|0|||Error|Console||-1||ConsoleSupervisor|") + \
	    std::string(__FILE__) + std::string("|") + std::to_string(__LINE__) +            \
	    std::string("|")
#define CONSOLE_SPECIAL_WARNING                                                    \
	std::string(                                                                   \
	    "|30-Aug-2019 15:30:17 CDT|0|||Warning|Console||-1||ConsoleSupervisor|") + \
	    std::string(__FILE__) + std::string("|") + std::to_string(__LINE__) +      \
	    std::string("|")

#undef __MF_SUBJECT__
#define __MF_SUBJECT__ "Console"

#define CONSOLE_MISSED_NEEDLE "Console missed * packet(s)"

///Count always happens, and System Message always happens for FSM commands
const std::set<std::string> ConsoleSupervisor::CUSTOM_TRIGGER_ACTIONS({"Count Only",
                                                                       "System Message",
                                                                       "Halt",
                                                                       "Stop",
                                                                       "Pause",
                                                                       "Soft Error",
                                                                       "Hard Error"});

const std::string ConsoleSupervisor::ConsoleMessageStruct::LABEL_TRACE      = "TRACE";
const std::string ConsoleSupervisor::ConsoleMessageStruct::LABEL_TRACE_PLUS = "TRACE+";
const std::map<ConsoleSupervisor::ConsoleMessageStruct::FieldType,
               std::string /* fieldNames */>
    ConsoleSupervisor::ConsoleMessageStruct::fieldNames({
        {FieldType::TIMESTAMP, "Timestamp"},
        {FieldType::SEQID, "SequenceID"},
        {FieldType::LEVEL, "Level"},
        {FieldType::LABEL, "Label"},
        {FieldType::SOURCEID, "SourceID"},
        {FieldType::HOSTNAME, "Hostname"},
        {FieldType::SOURCE, "Source"},
        {FieldType::FILE, "File"},
        {FieldType::LINE, "Line"},
        {FieldType::MSG, "Msg"},
    });

//==============================================================================
ConsoleSupervisor::ConsoleSupervisor(xdaq::ApplicationStub* stub)
    : CoreSupervisorBase(stub)
    , messageCount_(0)
    , maxMessageCount_(100000)
    , maxClientMessageRequest_(500)
{
	__SUP_COUT__ << "Constructor started." << __E__;

	INIT_MF("." /*directory used is USER_DATA/LOG/.*/);

	// attempt to make directory structure (just in case)
	mkdir(((std::string)USER_CONSOLE_PREF_PATH).c_str(), 0755);
	mkdir(((std::string)USER_CONSOLE_SNAPSHOT_PATH).c_str(), 0755);

	xoap::bind(
	    this, &ConsoleSupervisor::resetConsoleCounts, "ResetConsoleCounts", XDAQ_NS_URI);

	init();

	//test custom count list and always force the first one to be the "Console missed" !
	//Note: to avoid recursive triggers, Label='Console' can not trigger, so this 1st one is the only Console source trigger!
	//	Custom Trigger Philosophy
	//		- only one global custom count priority list (too complicated to manage by user, would have to timeout logins, etc..)
	//		- the admin users can modify priority of items in prority list
	//		- actions can infer others ('Count' is always inferred, in addition 'System Message' is inferred for FSM actions)
	//		- the admin users can modify actions
	//			-- including to the 1st Console-missing-message custom count
	loadCustomCountList();
	if(priorityCustomTriggerList_.size() ==
	   0)  //then create default starting list example for user
	{
		addCustomTriggeredAction(CONSOLE_MISSED_NEEDLE, "System Message");
		// addCustomTriggeredAction("runtime*4",
		// 	"Halt");
		// addCustomTriggeredAction("runtime",
		// 	"System Message");
	}  //end test and force custom count list

	__SUP_COUT__ << "Constructor complete." << __E__;

}  // end constructor()

//==============================================================================
ConsoleSupervisor::~ConsoleSupervisor(void) { destroy(); }
//==============================================================================
void ConsoleSupervisor::init(void)
{
	// start mf msg listener
	std::thread(
	    [](ConsoleSupervisor* cs) {
		    ConsoleSupervisor::messageFacilityReceiverWorkLoop(cs);
	    },
	    this)
	    .detach();
}  // end init()

//==============================================================================
void ConsoleSupervisor::destroy(void)
{
	// called by destructor
}  // end destroy()

//==============================================================================
xoap::MessageReference ConsoleSupervisor::resetConsoleCounts(
    xoap::MessageReference /*message*/)
{
	__COUT_INFO__ << "Resetting Console Error/Warn/Info counts and preparing for new "
	                 "first messages."
	              << __E__;

	// lockout the messages array for the remainder of the scope
	// this guarantees the reading thread can safely access the messages
	std::lock_guard<std::mutex> lock(messageMutex_);
	errorCount_            = 0;
	firstErrorMessageTime_ = 0;
	lastErrorMessageTime_  = 0;
	warnCount_             = 0;
	firstWarnMessageTime_  = 0;
	lastWarnMessageTime_   = 0;
	infoCount_             = 0;
	firstInfoMessageTime_  = 0;
	lastInfoMessageTime_   = 0;

	return SOAPUtilities::makeSOAPMessageReference("Done");
}  // end resetConsoleCounts()

//==============================================================================
/// messageFacilityReceiverWorkLoop ~~
///	Thread for printing Message Facility messages without decorations
///	Note: Uses std::mutex to avoid conflict with reading thread.
void ConsoleSupervisor::messageFacilityReceiverWorkLoop(ConsoleSupervisor* cs)
try
{
	__COUT__ << "Starting workloop based on config file: " << QUIET_CFG_FILE << __E__;

	std::string configFile = QUIET_CFG_FILE;
	FILE*       fp         = fopen(configFile.c_str(), "r");
	if(!fp)
	{
		__SS__ << "File with port info could not be loaded: " << QUIET_CFG_FILE << __E__;
		__COUT__ << "\n" << ss.str();
		__SS_THROW__;
	}
	char tmp[100];
	fgets(tmp, 100, fp);  // receive port (ignore)
	fgets(tmp, 100, fp);  // destination port *** used here ***
	int myport;
	sscanf(tmp, "%*s %d", &myport);

	fgets(tmp, 100, fp);  // destination ip *** used here ***
	char myip[100];
	sscanf(tmp, "%*s %s", myip);
	fclose(fp);

	ReceiverSocket rsock(myip, myport);  // Take Port from Configuration
	try
	{
		rsock.initialize(0x1400000 /*socketReceiveBufferSize*/);
	}
	catch(...)
	{
		// lockout the messages array for the remainder of the scope
		// this guarantees the reading thread can safely access the messages
		std::lock_guard<std::mutex> lock(cs->messageMutex_);

		// NOTE: if we do not want this to be fatal, do not throw here, just print out

		if(1)  // generate special message and throw for failed socket
		{
			__SS__ << "FATAL Console error. Could not initialize socket on port "
			       << myport
			       << ". Perhaps the port is already in use? Check for multiple stale "
			          "instances of otsdaq processes, or notify admins."
			       << " Multiple instances of otsdaq on the same node should be "
			          "possible, but port numbers must be unique."
			       << __E__;
			__SS_THROW__;
		}

		// generate special message to indicate failed socket
		__SS__ << "FATAL Console error. Could not initialize socket on port " << myport
		       << ". Perhaps it is already in use? Exiting Console receive loop."
		       << __E__;
		__COUT__ << ss.str();

		cs->messages_.emplace_back(CONSOLE_SPECIAL_ERROR + ss.str(),
		                           cs->messageCount_++,
		                           cs->priorityCustomTriggerList_);
		//force time to now for auto-generated message
		cs->messages_.back().setTime(time(0));
		if(cs->messages_.size() > cs->maxMessageCount_)
		{
			cs->messages_.erase(cs->messages_.begin());
		}

		return;
	}

	std::string buffer;
	int         i                         = 0;
	int         heartbeatCount            = 0;
	int         selfGeneratedMessageCount = 0;

	std::map<unsigned int, unsigned int> sourceLastSequenceID;  // map from sourceID to
	                                                            // lastSequenceID to
	// identify missed messages
	long long    newSourceId;
	uint32_t     newSequenceId;
	unsigned int c;

	// force a starting message
	__COUT__ << "DEBUG messages look like this." << __E__;

	while(1)
	{
		// if receive succeeds display message

		//__COUTV__(i);

		if(rsock.receive(
		       buffer, 1 /*timeoutSeconds*/, 0 /*timeoutUSeconds*/, false /*verbose*/) !=
		   -1)
		{
			// use 1-byte "ping" to keep socket alive
			if(buffer.size() == 1)
			{
				// std::cout << "Ping!" << __E__;
				continue;
			}

			if(i != 200)
			{
				__COUT__ << "Console has first message." << __E__;
				i = 200;  // mark so things are good for all time. (this indicates things
				          // are configured to be sent here)

				__COUT_INFO__ << "INFO messages look like this." << __E__;
				__COUT_WARN__ << "WARNING messages look like this." << __E__;
				__COUT_ERR__ << "ERROR messages look like this." << __E__;

				//				//to debug special packets
				//				__SS__ << "???";
				//				cs->messages_[cs->writePointer_].set(CONSOLE_SPECIAL_ERROR
				//+ 						ss.str(),
				//						cs->messageCount_++);
				//
				//				if(++cs->writePointer_ == cs->messages_.size()) //handle
				// wrap-around 					cs->writePointer_ = 0;
			}

			if(selfGeneratedMessageCount)
				--selfGeneratedMessageCount;  // decrement internal message count
			else  // reset heartbeat if external messages are coming through
				heartbeatCount = 0;

			//__COUT__ << buffer << __E__;

			// lockout the messages array for the remainder of the scope
			// this guarantees the reading thread can safely access the messages
			std::lock_guard<std::mutex> lock(cs->messageMutex_);

			// handle message stacking in packet
			c = 0;
			while(c < buffer.size())
			{
				// std::cout << "CONSOLE " << c << " sz=" << buffer.size() << " len=" <<
				// 	strlen(&(buffer.c_str()[c])) << __E__;
				cs->messages_.emplace_back(&(buffer.c_str()[c]),
				                           cs->messageCount_++,
				                           cs->priorityCustomTriggerList_);
				if(cs->messages_.back().hasCustomTriggerMatchAction())
					cs->customTriggerActionQueue_.push(
					    cs->messages_.back().getCustomTriggerMatch());

				//update system status
				if(cs->messages_.back().getLevel() == "Error")
				{
					if(cs->errorCount_ == 0)
					{
						cs->firstErrorMessageTime_ = time(0);
						cs->firstErrorMessage_     = cs->messages_.back().getMsg();
					}

					cs->lastErrorMessageTime_ = time(0);
					++cs->errorCount_;
					cs->lastErrorMessage_ = cs->messages_.back().getMsg();
				}
				else if(cs->messages_.back().getLevel() == "Warning")
				{
					if(cs->warnCount_ == 0)
					{
						cs->firstWarnMessageTime_ = time(0);
						cs->firstWarnMessage_     = cs->messages_.back().getMsg();
					}
					cs->lastWarnMessageTime_ = time(0);
					++cs->warnCount_;
					cs->lastWarnMessage_ = cs->messages_.back().getMsg();
				}
				else if(cs->messages_.back().getLevel() == "Info")
				{
					if(cs->infoCount_ == 0)
					{
						cs->firstInfoMessageTime_ = time(0);
						cs->firstInfoMessage_     = cs->messages_.back().getMsg();
					}
					cs->lastInfoMessageTime_ = time(0);
					++cs->infoCount_;
					cs->lastInfoMessage_ = cs->messages_.back().getMsg();
				}

				// check if sequence ID is out of order
				newSourceId   = cs->messages_.back().getSourceIDAsNumber();
				newSequenceId = cs->messages_.back().getSequenceIDAsNumber();

				// std::cout << rsock.getLastIncomingIPAddress() << ":" <<
				// 	rsock.getLastIncomingPort() << ":newSourceId: " << newSourceId <<
				// 	":" << newSequenceId << __E__;

				if(  // newSequenceId%1000 == 0 || //for debugging missed!
				    (newSourceId != -1 &&
				     sourceLastSequenceID.find(newSourceId) !=
				         sourceLastSequenceID
				             .end() &&  // ensure not first packet received
				     ((newSequenceId == 0 && sourceLastSequenceID[newSourceId] !=
				                                 (uint32_t)-1) ||  // wrap around case
				      newSequenceId !=
				          sourceLastSequenceID[newSourceId] + 1))  // normal sequence case
				)
				{
					// missed some messages!
					std::stringstream missedSs;
					missedSs << "Console missed "
					         << (newSequenceId - 1) -
					                (sourceLastSequenceID[newSourceId] + 1) + 1
					         << " packet(s) from " << cs->messages_.back().getSource()
					         << "!" << __E__;
					__SS__ << missedSs.str();
					std::cout << ss.str();

					// generate special message to indicate missed packets
					cs->messages_.emplace_back(CONSOLE_SPECIAL_WARNING + missedSs.str(),
					                           cs->messageCount_++,
					                           cs->priorityCustomTriggerList_);
					//force time to now for auto-generated message
					cs->messages_.back().setTime(time(0));
					//Force a custom count because Console Label are ignored! if(cs->messages_.back().hasCustomTriggerMatchAction())
					if(cs->priorityCustomTriggerList_.size())
					{
						cs->customTriggerActionQueue_.push(
						    cs->priorityCustomTriggerList_
						        [0]);  //push newest action to back
						cs->priorityCustomTriggerList_[0]
						    .occurrences++;  //increment occurrences
						cs->customTriggerActionQueue_.back().triggeredMessageCountIndex =
						    cs->messages_.back().getCount();
						cs->messages_.back().setCustomTriggerMatch(
						    cs->customTriggerActionQueue_.back());
					}
				}

				// save the new last sequence ID
				sourceLastSequenceID[newSourceId] = newSequenceId;

				while(cs->messages_.size() > 0 &&
				      cs->messages_.size() > cs->maxMessageCount_)
				{
					cs->messages_.erase(cs->messages_.begin());
				}

				c += strlen(&(buffer.c_str()[c])) + 1;
			}  // end handle message stacking in packet
		}      // end received packet handling
		else   // idle network handling
		{
			if(i < 120)  // if nothing received for 120 seconds, then something is wrong
			             // with Console configuration
				++i;

			sleep(1);  // sleep one second, if timeout

			// every 60 heartbeatCount (2 seconds each = 1 sleep and 1 timeout) print a
			// heartbeat message
			if(i != 200 ||  // show first message, if not already a message
			   (heartbeatCount < 60 * 5 &&
			    heartbeatCount % 60 == 59))  // every ~2 min for first 5 messages
			{
				++selfGeneratedMessageCount;  // increment internal message count
				__COUT__ << "Console is alive and waiting... (if no messages, next "
				            "heartbeat is in two minutes)"
				         << __E__;
			}
			else if(heartbeatCount % (60 * 30) == 59)  // approx every hour
			{
				++selfGeneratedMessageCount;  // increment internal message count
				__COUT__ << "Console is alive and waiting a long time... (if no "
				            "messages, next heartbeat is in one hour)"
				         << __E__;
			}

			++heartbeatCount;
		}  // end idle network handling

		// if nothing received for 2 minutes seconds, then something is wrong with Console
		// configuration 	after 5 seconds there is a self-send. Which will at least
		// confirm configuration. 	OR if 5 generated messages and never cleared.. then
		// the forwarding is not working.
		if(i == 120 || selfGeneratedMessageCount == 5)
		{
			__COUTV__(i);
			__COUTV__(selfGeneratedMessageCount);
			__COUT__ << "No messages received at Console Supervisor. Exiting Console "
			            "messageFacilityReceiverWorkLoop"
			         << __E__;
			break;  // assume something wrong, and break loop
		}

		if(cs->customTriggerActionQueue_.size() && !cs->customTriggerActionThreadExists_)
		{
			cs->customTriggerActionThreadExists_ = true;

			std::thread(
			    [](ConsoleSupervisor* c) {
				    ConsoleSupervisor::customTriggerActionThread(c);
			    },
			    cs)
			    .detach();
		}

	}  // end infinite loop

}  // end messageFacilityReceiverWorkLoop()
catch(const std::runtime_error& e)
{
	__COUT_ERR__ << "Error caught at Console Supervisor thread: " << e.what() << __E__;
}
catch(...)
{
	__COUT_ERR__ << "Unknown error caught at Console Supervisor thread." << __E__;
}  // end messageFacilityReceiverWorkLoop() exception handling

//==============================================================================
/// customTriggerActionThread ~~
///	Thread for sequentially handling customTriggerActionQueue_
void ConsoleSupervisor::customTriggerActionThread(ConsoleSupervisor* cs)
try
{
	__COUT__ << "Starting customTriggerActionThread" << __E__;
	CustomTriggeredAction_t triggeredAction;
	while(1)  //infinite workloop
	{
		{  //mutex scope:
			// lockout the messages array for the remainder of the scope
			// this guarantees the reading thread can safely access the action queue
			std::lock_guard<std::mutex> lock(cs->messageMutex_);
			if(cs->customTriggerActionQueue_.size())
			{
				triggeredAction = cs->customTriggerActionQueue_.front();
				cs->customTriggerActionQueue_.pop();  //pop first/oldest element
			}
		}  //end mutex scope

		if(triggeredAction.action.size())
		{
			__COUTS__(2) << "Handling action '" << triggeredAction.action
			             << "' on custom count search string: "
			             << StringMacros::vectorToString(triggeredAction.needleSubstrings,
			                                             {'*'})
			             << __E__;
			cs->doTriggeredAction(triggeredAction);
		}

		triggeredAction.action = "";  //clear action for next in queue
		triggeredAction.triggeredMessageCountIndex =
		    -1;    //clear triggered message ID for next in queue
		sleep(2);  //mostly sleep

	}  //end infinite workloop

}  // end customTriggerActionThread()
catch(const std::runtime_error& e)
{
	__COUT_ERR__ << "Error caught at Console Supervisor Action thread: " << e.what()
	             << __E__;
}
catch(...)
{
	__COUT_ERR__ << "Unknown error caught at Console Supervisor Action thread." << __E__;
}  // end customTriggerActionThread() exception handling

//==============================================================================
void ConsoleSupervisor::doTriggeredAction(const CustomTriggeredAction_t& triggeredAction)
{
	__SUP_COUT_INFO__ << "Launching Triggered Action '" << triggeredAction.action
	                  << "' fired on custom count search string: "
	                  << StringMacros::vectorToString(triggeredAction.needleSubstrings,
	                                                  {'*'})
	                  << __E__;

	//valid actions:
	//		Halt
	//	 	Stop
	//		Pause
	//		Soft Error
	//		Hard Error
	//		System Message

	if(CUSTOM_TRIGGER_ACTIONS.find(triggeredAction.action) ==
	   CUSTOM_TRIGGER_ACTIONS.end())
	{
		__SUP_SS__ << "Unrecognized triggered action '" << triggeredAction.action
		           << ",' valid actions are "
		           << StringMacros::setToString(CUSTOM_TRIGGER_ACTIONS) << __E__;
		__SUP_SS_THROW__;
	}

	//all FSM commands include a system message
	if(triggeredAction.action != "Count Only")
		theRemoteWebUsers_.sendSystemMessage(
		    "*" /* to all users*/,
		    "In the Console Supervisor, a custom count fired the action '" +
		        triggeredAction.action + "' on the search string '" +
		        StringMacros::vectorToString(triggeredAction.needleSubstrings, {'*'}) +
		        "'");

	if(triggeredAction.action == "Halt")
	{
		//TODO
	}
	else if(triggeredAction.action == "Stop")
	{
		//TODO
	}
	else if(triggeredAction.action == "Pause")
	{
		//TODO
	}

}  // end doTriggeredAction()

//==============================================================================
///Never allow priority 0 to change, forced to be missed packets
void ConsoleSupervisor::addCustomTriggeredAction(const std::string& triggerNeedle,
                                                 const std::string& triggerAction,
												 uint32_t           priority, /* = -1 */
												 uint32_t  			triggerOnCount,
												 bool  				doLoop)
{
	__SUP_COUTV__(triggerNeedle);

	bool allAsterisks = true;
	for(const auto& c : triggerNeedle)
		if(c != '*')
		{
			allAsterisks = false;
			break;
		}
	if(allAsterisks)
	{
		__SUP_SS__ << "Illegal empty Search String value for the new Custom Count and "
		              "Action! Please enter a valid Search String (* wildcards are "
		              "allowed, e.g. \"value = * seconds\")."
		           << __E__;
		__SUP_SS_THROW__;
	}

	//check if triggerNeedle already exists
	uint32_t currentPriority = -1;
	for(const auto& customTrigger : priorityCustomTriggerList_)
	{
		++currentPriority;  //inc first to get to 0
		if(StringMacros::vectorToString(customTrigger.needleSubstrings, {'*'}) ==
		   triggerNeedle)
		{
			__SUP_SS__ << "Failure! Can not add Custom Count Search String that already "
			              "exists. Found '"
			           << triggerNeedle
			           << "' already existing at priority = " << currentPriority << __E__;
			__SUP_SS_THROW__;
		}
	}  //end check if already exists

	__SUP_COUTV__(triggerAction);
	__SUP_COUTV__(priority);
	if(priority >= priorityCustomTriggerList_.size())
		priority = priorityCustomTriggerList_.size();  //place at end
	if(priority == 0 && triggerNeedle != CONSOLE_MISSED_NEEDLE)
	{
		__SUP_SS__ << "Illegal priority position of '" << priority
		           << "' requested. Please enter a priority value greater than 0. "
		              "Position 0 is reserved for identifying missing messages at the "
		              "Console Supervisor. Note: the action for missing messages, at "
		              "priority 0, may be customized by the user."
		           << __E__;
		__SUP_SS_THROW__;
	}

	//valid actions:
	//		Halt
	//	 	Stop
	//		Pause
	//		Soft Error
	//		Hard Error
	//		System Message

	if(CUSTOM_TRIGGER_ACTIONS.find(triggerAction) == CUSTOM_TRIGGER_ACTIONS.end())
	{
		__SUP_SS__ << "Unrecognized triggered action '" << triggerAction
		           << ",' valid actions are "
		           << StringMacros::setToString(CUSTOM_TRIGGER_ACTIONS) << __E__;
		__SUP_SS_THROW__;
	}

	//insert new custom count at priority position
	priorityCustomTriggerList_.insert(priorityCustomTriggerList_.begin() + priority,
	                                  CustomTriggeredAction_t());
	// priorityCustomTriggerList_.push_back(CustomTriggeredAction_t());
	// priorityCustomTriggerList_.back()

	//break up on substring
	StringMacros::getVectorFromString(
	    triggerNeedle,
	    priorityCustomTriggerList_[priority].needleSubstrings,
	    {'*'} /* delimiter */,
	    {} /* do not ignore whitespace */);
	priorityCustomTriggerList_[priority].action = triggerAction;
	priorityCustomTriggerList_[priority].triggerOnCount = triggerOnCount;
	priorityCustomTriggerList_[priority].doLoop = doLoop;

	__SUP_COUT__ << "Added custom count: "
	             << (StringMacros::vectorToString(
	                    priorityCustomTriggerList_[priority].needleSubstrings))
	             << " at priority: " << priority
				 << " triggered every: " << triggerOnCount << " occurrences";
	if(doLoop)
		__SUP_COUT__ << " and will loop.";
	else
		__SUP_COUT__ << " and will not loop.";
	__SUP_COUT__ << __E__;

}  // end addCustomTriggeredAction()

//==============================================================================
///Never allow priority 0 to change, forced to be missed packets
/// modifyType := {"Deletion", "Priority", "Action","Search String", "All"}
/// Returns the modified priority position, or -1 on deletion
uint32_t ConsoleSupervisor::modifyCustomTriggeredAction(const std::string& currentNeedle,
                                                        const std::string& modifyType,
                                                        const std::string& setNeedle,
                                                        const std::string& setAction,
                                                        uint32_t           setPriority,
														uint32_t setTriggerOnCount,
														bool   setDoLoop)
{
	__SUP_COUTV__(currentNeedle);
	__SUP_COUTV__(modifyType);
	__SUP_COUTV__(setNeedle);
	__SUP_COUTV__(setAction);
	__SUP_COUTV__(setPriority);
	__SUP_COUTV__(setTriggerOnCount);
	__SUP_COUTV__(setDoLoop);

	//find current priority position of currentNeedle
	uint32_t currentPriority = -1;
	bool     found           = false;
	for(const auto& customTrigger : priorityCustomTriggerList_)
	{
		++currentPriority;  //inc first to get to 0, -1 indicates not found
		if(StringMacros::vectorToString(customTrigger.needleSubstrings, {'*'}) ==
		   currentNeedle)
		{
			found = true;
			break;  //found
		}
	}

	__SUP_COUTV__(currentPriority);
	if(!found)
	{
		__SUP_SS__ << "Attempt to modify Custom Count Search String failed. Could not "
		              "find specified Search String '"
		           << currentNeedle << "' in prioritized list." << __E__;
		__SUP_SS_THROW__;
	}

	if(modifyType == "Deletion")
	{
		if(currentPriority == 0)
		{
			__SUP_SS__ << "Illegal deletion requested of priority position 0. Position 0 "
			              "is reserved for identifying missing messages at the Console "
			              "Supervisor. Note: the action of priority 0 may be customized "
			              "by the user, but it can not be deleted."
			           << __E__;
			__SUP_SS_THROW__;
		}

		__SUP_COUT__ << "Deleting custom count: "
		             << StringMacros::vectorToString(
		                    priorityCustomTriggerList_[currentPriority].needleSubstrings,
		                    {'*'})
		             << " w/action: "
		             << priorityCustomTriggerList_[currentPriority].action
		             << " and priority: " << currentPriority << __E__;
		priorityCustomTriggerList_.erase(priorityCustomTriggerList_.begin() +
		                                 currentPriority);
		return -1;
	}

	if(modifyType == "Priority" || modifyType == "All")
	{
		if(setPriority >= priorityCustomTriggerList_.size())
			setPriority = priorityCustomTriggerList_.size();  //place at end
		if(setPriority == 0 && setNeedle != CONSOLE_MISSED_NEEDLE)
		{
			__SUP_SS__ << "Illegal priority position of '" << setPriority
			           << "' requested. Position 0 is reserved for identifying missing "
			              "messages at the Console Supervisor. Note: the action of "
			              "priority 0 may be customized by the user."
			           << __E__;
			__SUP_SS_THROW__;
		}
	}
	else  //keep existing
		setPriority = currentPriority;

	if(modifyType == "Action" || modifyType == "All")
	{
		//valid actions:
		//		Halt
		//	 	Stop
		//		Pause
		//		Soft Error
		//		Hard Error
		//		System Message

		if(CUSTOM_TRIGGER_ACTIONS.find(setAction) == CUSTOM_TRIGGER_ACTIONS.end())
		{
			__SUP_SS__ << "Unrecognized custom count action '" << setAction
			           << ",' valid actions are "
			           << StringMacros::setToString(CUSTOM_TRIGGER_ACTIONS) << __E__;
			__SUP_SS_THROW__;
		}
		//modify existing action
		priorityCustomTriggerList_[currentPriority].action = setAction;
	}

	if(modifyType == "Search String" || modifyType == "All")
	{
		//modify existing needle
		priorityCustomTriggerList_[currentPriority].needleSubstrings.clear();
		StringMacros::getVectorFromString(
		    setNeedle,
		    priorityCustomTriggerList_[currentPriority].needleSubstrings,
		    {'*'} /* delimiter */,
		    {} /* do not ignore whitespace */);
	}

	if(modifyType == "Trigger on Count" || modifyType == "All")
	{
		//modify existing action
		priorityCustomTriggerList_[currentPriority].triggerOnCount = setTriggerOnCount;
	}
	if(modifyType == "Do Loop" || modifyType == "All")
	{
		//modify existing action
		priorityCustomTriggerList_[currentPriority].doLoop = setDoLoop;
	}

	if(currentPriority != setPriority)  //then need to copy
	{
		//insert new custom count at priority position
		priorityCustomTriggerList_.insert(
		    priorityCustomTriggerList_.begin() + setPriority,
		    priorityCustomTriggerList_[currentPriority]);

		//delete from old position
		if(currentPriority >= setPriority)  //then increment after insert
			++currentPriority;

		priorityCustomTriggerList_.erase(priorityCustomTriggerList_.begin() +
		                                 currentPriority);

		if(currentPriority < setPriority)  //then decrement after delete
			--setPriority;
	}

	__SUP_COUT__ << "Modified '" << modifyType << "' custom count: "
	             << StringMacros::vectorToString(
	                    priorityCustomTriggerList_[setPriority].needleSubstrings, {'*'})
	             << " now w/action: " << priorityCustomTriggerList_[setPriority].action
	             << " and at priority: " << setPriority << __E__;

	return setPriority;
}  // end modifyCustomTriggeredAction()

//==============================================================================
void ConsoleSupervisor::loadCustomCountList()
{
	__SUP_COUT__ << "loadCustomCountList() from "
	             << USER_CONSOLE_PREF_PATH + CUSTOM_COUNT_LIST_FILENAME << __E__;

	FILE* fp = fopen((USER_CONSOLE_PREF_PATH + CUSTOM_COUNT_LIST_FILENAME).c_str(), "r");
	if(!fp)
	{
		__SUP_COUT__ << "Ignoring missing Custom Count list file at path: "
		             << (USER_CONSOLE_PREF_PATH + CUSTOM_COUNT_LIST_FILENAME) << __E__;
		return;
	}
	priorityCustomTriggerList_.clear();

	char        line[1000];  //do not allow larger than 1000 chars!
	uint32_t    i = 0;
	std::string needle;
	while(fgets(line, 1000, fp))
	{
		//ignore new line
		if(strlen(line))
			line[strlen(line) - 1] = '\0';

		if(i % 2 == 0)  //needle
			needle = line;
		else  //action (so have all info)
		{
			__SUP_COUTTV__(needle);
			__SUP_COUTTV__(line);
			if(i == 1 &&
			   needle !=
			       CONSOLE_MISSED_NEEDLE)  //then force missed Console message as priority 0
				addCustomTriggeredAction(CONSOLE_MISSED_NEEDLE, "System Message");
			addCustomTriggeredAction(needle, line);
		}

		++i;
	}
	fclose(fp);

}  // end loadCustomCountList()

//==============================================================================
void ConsoleSupervisor::saveCustomCountList()
{
	__SUP_COUT__ << "saveCustomCountList()" << __E__;

	FILE* fp = fopen((USER_CONSOLE_PREF_PATH + CUSTOM_COUNT_LIST_FILENAME).c_str(), "w");
	if(!fp)
	{
		__SUP_SS__ << "Failed to create Custom Count list file at path: "
		           << (USER_CONSOLE_PREF_PATH + CUSTOM_COUNT_LIST_FILENAME) << __E__;
		__SUP_SS_THROW__;
	}
	for(auto& customCount : priorityCustomTriggerList_)
	{
		fprintf(fp,
		        (StringMacros::vectorToString(customCount.needleSubstrings, {'*'}) + "\n")
		            .c_str());
		fprintf(fp, (customCount.action + "\n").c_str());
	}
	fclose(fp);
}  // end saveCustomCountList()

//==============================================================================
void ConsoleSupervisor::defaultPage(xgi::Input* /*in*/, xgi::Output* out)
{
	// __SUP_COUT__ << "ApplicationDescriptor LID="
	//              << getApplicationDescriptor()->getLocalId() << __E__;
	*out << "<!DOCTYPE HTML><html lang='en'><frameset col='100%' row='100%'><frame "
	        "src='/WebPath/html/Console.html?urn="
	     << getApplicationDescriptor()->getLocalId() << "'></frameset></html>";
}  // end defaultPage()

//==============================================================================
/// forceSupervisorPropertyValues
///		override to force supervisor property values (and ignore user settings)
void ConsoleSupervisor::forceSupervisorPropertyValues()
{
	CorePropertySupervisorBase::setSupervisorProperty(
	    CorePropertySupervisorBase::SUPERVISOR_PROPERTIES.AutomatedRequestTypes,
	    "GetConsoleMsgs");
}  // end forceSupervisorPropertyValues()

//==============================================================================
///	Request
///		Handles Web Interface requests to Console supervisor.
///		Does not refresh cookie for automatic update checks.
void ConsoleSupervisor::request(const std::string&               requestType,
                                cgicc::Cgicc&                    cgiIn,
                                HttpXmlDocument&                 xmlOut,
                                const WebUsers::RequestUserInfo& userInfo)
{
	//__SUP_COUT__ << "requestType " << requestType << __E__;

	// Commands:
	// GetConsoleMsgs
	// PrependHistoricMessages
	// SaveUserPreferences
	// LoadUserPreferences
	// GetTraceLevels
	// SetTraceLevels
	// GetTriggerStatus
	// SetTriggerEnable
	// ResetTRACE
	// EnableTRACE
	// GetTraceSnapshot
	// GetCustomCountsAndActions
	// AddCustomCountsAndAction
	// ModifyCustomCountsAndAction

	// Note: to report to logbook admin status use
	// xmlOut.addTextElementToData(XML_ADMIN_STATUS,refreshTempStr_);

	if(requestType == "GetConsoleMsgs")
	{
		// lindex of -1 means first time and user just gets update lcount and lindex
		std::string lastUpdateCountStr = CgiDataUtilities::postData(cgiIn, "lcount");

		if(lastUpdateCountStr == "")
		{
			__SUP_COUT_ERR__ << "Invalid Parameters! lastUpdateCount="
			                 << lastUpdateCountStr << __E__;
			xmlOut.addTextElementToData("Error",
			                            "Error - Invalid parameters for GetConsoleMsgs.");
			return;
		}

		size_t lastUpdateCount = std::stoull(lastUpdateCountStr);

		//		__SUP_COUT__ << "lastUpdateCount=" << lastUpdateCount << __E__;

		insertMessageRefresh(&xmlOut, lastUpdateCount);
	}
	else if(requestType == "PrependHistoricMessages")
	{
		size_t earliestOnhandMessageCount =
		    CgiDataUtilities::postDataAsInt(cgiIn, "earlyCount");
		__SUP_COUTV__(earliestOnhandMessageCount);
		prependHistoricMessages(&xmlOut, earliestOnhandMessageCount);
	}
	else if(requestType == "SaveUserPreferences")
	{
		int colorIndex     = CgiDataUtilities::postDataAsInt(cgiIn, "colorIndex");
		int showSideBar    = CgiDataUtilities::postDataAsInt(cgiIn, "showSideBar");
		int noWrap         = CgiDataUtilities::postDataAsInt(cgiIn, "noWrap");
		int messageOnly    = CgiDataUtilities::postDataAsInt(cgiIn, "messageOnly");
		int hideLineNumers = CgiDataUtilities::postDataAsInt(cgiIn, "hideLineNumers");

		// __SUP_COUT__ << "requestType " << requestType << __E__;
		// __SUP_COUT__ << "colorIndex: " << colorIndex << __E__;
		// __SUP_COUT__ << "showSideBar: " << showSideBar << __E__;
		// __SUP_COUT__ << "noWrap: " << noWrap << __E__;
		// __SUP_COUT__ << "messageOnly: " << messageOnly << __E__;
		// __SUP_COUT__ << "hideLineNumers: " << hideLineNumers << __E__;

		if(userInfo.username_ == "")  // should never happen?
		{
			__SUP_COUT_ERR__ << "Invalid user found! user=" << userInfo.username_
			                 << __E__;
			xmlOut.addTextElementToData("Error",
			                            "Error - InvauserInfo.username_user found.");
			return;
		}

		std::string fn = (std::string)USER_CONSOLE_PREF_PATH + userInfo.username_ + "." +
		                 (std::string)USERS_PREFERENCES_FILETYPE;

		// __SUP_COUT__ << "Save preferences: " << fn << __E__;
		FILE* fp = fopen(fn.c_str(), "w");
		if(!fp)
		{
			__SS__;
			__THROW__(ss.str() + "Could not open file: " + fn);
		}
		fprintf(fp, "colorIndex %d\n", colorIndex);
		fprintf(fp, "showSideBar %d\n", showSideBar);
		fprintf(fp, "noWrap %d\n", noWrap);
		fprintf(fp, "messageOnly %d\n", messageOnly);
		fprintf(fp, "hideLineNumers %d\n", hideLineNumers);
		fclose(fp);
	}
	else if(requestType == "LoadUserPreferences")
	{
		// __SUP_COUT__ << "requestType " << requestType << __E__;

		unsigned int colorIndex, showSideBar, noWrap, messageOnly, hideLineNumers;

		if(userInfo.username_ == "")  // should never happen?
		{
			__SUP_COUT_ERR__ << "Invalid user found! user=" << userInfo.username_
			                 << __E__;
			xmlOut.addTextElementToData("Error", "Error - Invalid user found.");
			return;
		}

		std::string fn = (std::string)USER_CONSOLE_PREF_PATH + userInfo.username_ + "." +
		                 (std::string)USERS_PREFERENCES_FILETYPE;

		// __SUP_COUT__ << "Load preferences: " << fn << __E__;

		FILE* fp = fopen(fn.c_str(), "r");
		if(!fp)
		{
			// return defaults
			__SUP_COUT__ << "Returning defaults." << __E__;
			xmlOut.addTextElementToData("colorIndex", "0");
			xmlOut.addTextElementToData("showSideBar", "0");
			xmlOut.addTextElementToData("noWrap", "1");
			xmlOut.addTextElementToData("messageOnly", "0");
			xmlOut.addTextElementToData("hideLineNumers", "1");
			return;
		}
		fscanf(fp, "%*s %u", &colorIndex);
		fscanf(fp, "%*s %u", &showSideBar);
		fscanf(fp, "%*s %u", &noWrap);
		fscanf(fp, "%*s %u", &messageOnly);
		fscanf(fp, "%*s %u", &hideLineNumers);
		fclose(fp);
		// __SUP_COUT__ << "colorIndex: " << colorIndex << __E__;
		// __SUP_COUT__ << "showSideBar: " << showSideBar << __E__;
		// __SUP_COUT__ << "noWrap: " << noWrap << __E__;
		// __SUP_COUT__ << "messageOnly: " << messageOnly << __E__;
		// __SUP_COUT__ << "hideLineNumers: " << hideLineNumers << __E__;

		char tmpStr[20];
		sprintf(tmpStr, "%u", colorIndex);
		xmlOut.addTextElementToData("colorIndex", tmpStr);
		sprintf(tmpStr, "%u", showSideBar);
		xmlOut.addTextElementToData("showSideBar", tmpStr);
		sprintf(tmpStr, "%u", noWrap);
		xmlOut.addTextElementToData("noWrap", tmpStr);
		sprintf(tmpStr, "%u", messageOnly);
		xmlOut.addTextElementToData("messageOnly", tmpStr);
		sprintf(tmpStr, "%u", hideLineNumers);
		xmlOut.addTextElementToData("hideLineNumers", tmpStr);
	}
	else if(requestType == "GetTraceLevels")
	{
		__SUP_COUT__ << "requestType " << requestType << __E__;

		SOAPParameters txParameters;  // params for xoap to send
		txParameters.addParameter("Request", "GetTraceLevels");

		SOAPParameters rxParameters;  // params for xoap to recv
		rxParameters.addParameter("Command");
		rxParameters.addParameter("Error");
		rxParameters.addParameter("TRACEHostnameList");
		rxParameters.addParameter("TRACEList");

		traceMapToXDAQHostname_.clear();  // reset

		std::string traceList = "";
		auto& allTraceApps    = allSupervisorInfo_.getAllTraceControllerSupervisorInfo();
		for(const auto& appInfo : allTraceApps)
		{
			__SUP_COUT__ << "Supervisor hostname = " << appInfo.first << "/"
			             << appInfo.second.getId()
			             << " name = " << appInfo.second.getName()
			             << " class = " << appInfo.second.getClass()
			             << " hostname = " << appInfo.second.getHostname() << __E__;
			try
			{
				xoap::MessageReference retMsg =
				    SOAPMessenger::sendWithSOAPReply(appInfo.second.getDescriptor(),
				                                     "TRACESupervisorRequest",
				                                     txParameters);
				SOAPUtilities::receive(retMsg, rxParameters);
				__SUP_COUT__ << "Received TRACE response: "
				             << SOAPUtilities::translate(retMsg).getCommand() << " ==> "
				             << SOAPUtilities::translate(retMsg) << __E__;

				if(SOAPUtilities::translate(retMsg).getCommand() == "Fault")
				{
					__SUP_SS__ << "Unrecognized command at destination TRACE Supervisor "
					              "hostname = "
					           << appInfo.first << "/" << appInfo.second.getId()
					           << " name = " << appInfo.second.getName()
					           << " class = " << appInfo.second.getClass()
					           << " hostname = " << appInfo.second.getHostname() << __E__;
					__SUP_SS_THROW__;
				}
				else if(SOAPUtilities::translate(retMsg).getCommand() == "TRACEFault")
				{
					__SUP_SS__ << "Error received: " << rxParameters.getValue("Error")
					           << __E__;
					__SUP_SS_THROW__;
				}
			}
			catch(const xdaq::exception::Exception& e)
			{
				__SUP_SS__ << "Error transmitting request to TRACE Supervisor LID = "
				           << appInfo.second.getId()
				           << " name = " << appInfo.second.getName() << ". \n\n"
				           << e.what() << __E__;
				//do not throw exception, because unable to set levels when some Supervisors are down
				//__SUP_SS_THROW__;
				__SUP_COUT_ERR__ << ss.str();
				continue;  //skip bad Supervisor
			}

			std::vector<std::string> traceHostnameArr;
			__COUTTV__(rxParameters.getValue("TRACEHostnameList"));
			StringMacros::getVectorFromString(
			    rxParameters.getValue("TRACEHostnameList"), traceHostnameArr, {';'});
			for(const auto& traceHostname : traceHostnameArr)
			{
				if(traceHostname == "")
					continue;  //skip blanks
				traceMapToXDAQHostname_[traceHostname] = appInfo.first;
			}

			// traceList 		  += ";" + appInfo.first; //insert xdaq context version of
			// name
			//						//FIXME and create mapp from user's typed in xdaq
			// context name to TRACE hostname resolution

			__COUTTV__(rxParameters.getValue("TRACEList"));
			traceList += rxParameters.getValue("TRACEList");

		}  // end app get TRACE loop
		__SUP_COUT__ << "TRACE hostname map received: \n"
		             << StringMacros::mapToString(traceMapToXDAQHostname_) << __E__;
		__SUP_COUT__ << "TRACE List received: \n" << traceList << __E__;
		xmlOut.addTextElementToData("traceList", traceList);
	}  // end GetTraceLevels
	else if(requestType == "SetTraceLevels")
	{
		__SUP_COUT__ << "requestType " << requestType << __E__;

		std::string individualValues =
		    CgiDataUtilities::postData(cgiIn, "individualValues");
		std::string hostLabelMap = CgiDataUtilities::postData(cgiIn, "hostLabelMap");
		std::string setMode      = CgiDataUtilities::postData(cgiIn, "setMode");
		std::string setValueMSB  = CgiDataUtilities::postData(cgiIn, "setValueMSB");
		std::string setValueLSB  = CgiDataUtilities::postData(cgiIn, "setValueLSB");

		__SUP_COUTV__(individualValues);
		__SUP_COUTV__(setMode);
		// set modes: SLOW, FAST, TRIGGER
		__SUP_COUTV__(setValueMSB);
		__SUP_COUTV__(setValueLSB);

		std::map<std::string /*host*/, std::string /*labelArr*/> hostToLabelMap;

		auto& allTraceApps = allSupervisorInfo_.getAllTraceControllerSupervisorInfo();

		SOAPParameters rxParameters;  // params for xoap to recv
		rxParameters.addParameter("Command");
		rxParameters.addParameter("Error");
		rxParameters.addParameter("TRACEList");

		std::string modifiedTraceList = "";
		std::string xdaqHostname;
		StringMacros::getMapFromString(hostLabelMap, hostToLabelMap, {';'}, {':'});
		for(auto& hostLabelsPair : hostToLabelMap)
		{
			// identify artdaq hosts to go through ARTDAQ supervisor
			//	by adding "artdaq.." to hostname artdaq..correlator2.fnal.gov
			__SUP_COUTV__(hostLabelsPair.first);
			__SUP_COUTV__(hostLabelsPair.second);

			// use map to convert to xdaq host
			try
			{
				xdaqHostname = traceMapToXDAQHostname_.at(hostLabelsPair.first);
			}
			catch(...)
			{
				__SUP_SS__ << "Could not find the translation from TRACE hostname '"
				           << hostLabelsPair.first << "' to xdaq Context hostname."
				           << __E__;
				ss << "Here is the existing map (size=" << traceMapToXDAQHostname_.size()
				   << "): " << StringMacros::mapToString(traceMapToXDAQHostname_)
				   << __E__;
				__SUP_SS_THROW__;
			}

			__SUP_COUTV__(xdaqHostname);

			auto& appInfo = allTraceApps.at(xdaqHostname);
			__SUP_COUT__ << "Supervisor hostname = " << hostLabelsPair.first << "/"
			             << xdaqHostname << ":" << appInfo.getId()
			             << " name = " << appInfo.getName()
			             << " class = " << appInfo.getClass()
			             << " hostname = " << appInfo.getHostname() << __E__;
			try
			{
				SOAPParameters txParameters;  // params for xoap to send
				txParameters.addParameter("Request", "SetTraceLevels");
				txParameters.addParameter("IndividualValues", individualValues);
				txParameters.addParameter("Host", hostLabelsPair.first);
				txParameters.addParameter("SetMode", setMode);
				txParameters.addParameter("Labels", hostLabelsPair.second);
				txParameters.addParameter("SetValueMSB", setValueMSB);
				txParameters.addParameter("SetValueLSB", setValueLSB);

				xoap::MessageReference retMsg = SOAPMessenger::sendWithSOAPReply(
				    appInfo.getDescriptor(), "TRACESupervisorRequest", txParameters);
				SOAPUtilities::receive(retMsg, rxParameters);
				__SUP_COUT__ << "Received TRACE response: "
				             << SOAPUtilities::translate(retMsg).getCommand() << " ==> "
				             << SOAPUtilities::translate(retMsg) << __E__;

				if(SOAPUtilities::translate(retMsg).getCommand() == "Fault")
				{
					__SUP_SS__ << "Unrecognized command at destination TRACE Supervisor "
					              "hostname = "
					           << hostLabelsPair.first << "/" << appInfo.getId()
					           << " name = " << appInfo.getName()
					           << " class = " << appInfo.getClass()
					           << " hostname = " << appInfo.getHostname() << __E__;
					__SUP_SS_THROW__;
				}
				else if(SOAPUtilities::translate(retMsg).getCommand() == "TRACEFault")
				{
					__SUP_SS__ << "Error received: " << rxParameters.getValue("Error")
					           << __E__;
					__SUP_SS_THROW__;
				}
			}
			catch(const xdaq::exception::Exception& e)
			{
				__SUP_SS__ << "Error transmitting request to TRACE Supervisor LID = "
				           << appInfo.getId() << " name = " << appInfo.getName()
				           << ". \n\n"
				           << e.what() << __E__;
				__SUP_SS_THROW__;
			}

			modifiedTraceList +=
			    ";" + hostLabelsPair.first;  // insert xdaq context version of name
			// FIXME and create mapp from user's typed in xdaq
			// context name to TRACE hostname resolution

			modifiedTraceList += rxParameters.getValue("TRACEList");

		}  // end host set TRACE loop

		__SUP_COUT__ << "mod'd TRACE List received: \n" << modifiedTraceList << __E__;
		xmlOut.addTextElementToData("modTraceList", modifiedTraceList);
	}  // end SetTraceLevels
	else if(requestType == "GetTriggerStatus")
	{
		__SUP_COUT__ << "requestType " << requestType << __E__;
		SOAPParameters txParameters;  // params for xoap to send
		txParameters.addParameter("Request", "GetTriggerStatus");

		SOAPParameters rxParameters;  // params for xoap to recv
		rxParameters.addParameter("Command");
		rxParameters.addParameter("Error");
		rxParameters.addParameter("TRACETriggerStatus");

		std::string traceTriggerStatus = "";
		auto& allTraceApps = allSupervisorInfo_.getAllTraceControllerSupervisorInfo();
		for(const auto& appInfo : allTraceApps)
		{
			__SUP_COUT__ << "Supervisor hostname = " << appInfo.first << "/"
			             << appInfo.second.getId()
			             << " name = " << appInfo.second.getName()
			             << " class = " << appInfo.second.getClass()
			             << " hostname = " << appInfo.second.getHostname() << __E__;
			try
			{
				xoap::MessageReference retMsg =
				    SOAPMessenger::sendWithSOAPReply(appInfo.second.getDescriptor(),
				                                     "TRACESupervisorRequest",
				                                     txParameters);
				SOAPUtilities::receive(retMsg, rxParameters);
				__SUP_COUT__ << "Received TRACE response: "
				             << SOAPUtilities::translate(retMsg).getCommand() << " ==> "
				             << SOAPUtilities::translate(retMsg) << __E__;

				if(SOAPUtilities::translate(retMsg).getCommand() == "Fault")
				{
					__SUP_SS__ << "Unrecognized command at destination TRACE Supervisor "
					              "hostname = "
					           << appInfo.first << "/" << appInfo.second.getId()
					           << " name = " << appInfo.second.getName()
					           << " class = " << appInfo.second.getClass()
					           << " hostname = " << appInfo.second.getHostname() << __E__;
					__SUP_SS_THROW__;
				}
				else if(SOAPUtilities::translate(retMsg).getCommand() == "TRACEFault")
				{
					__SUP_SS__ << "Error received: " << rxParameters.getValue("Error")
					           << __E__;
					__SUP_SS_THROW__;
				}
			}
			catch(const xdaq::exception::Exception& e)
			{
				__SUP_SS__ << "Error transmitting request to TRACE Supervisor LID = "
				           << appInfo.second.getId()
				           << " name = " << appInfo.second.getName() << ". \n\n"
				           << e.what() << __E__;
				__SUP_SS_THROW__;
			}

			traceTriggerStatus += rxParameters.getValue("TRACETriggerStatus");

		}  // end app get TRACE loop
		__SUP_COUT__ << "TRACE Trigger Status received: \n"
		             << traceTriggerStatus << __E__;
		xmlOut.addTextElementToData("traceTriggerStatus", traceTriggerStatus);
	}  // end GetTriggerStatus
	else if(requestType == "SetTriggerEnable")
	{
		__SUP_COUT__ << "requestType " << requestType << __E__;

		std::string hostList = CgiDataUtilities::postData(cgiIn, "hostList");

		__SUP_COUTV__(hostList);

		std::vector<std::string /*host*/> hosts;

		auto& allTraceApps = allSupervisorInfo_.getAllTraceControllerSupervisorInfo();

		SOAPParameters rxParameters;  // params for xoap to recv
		rxParameters.addParameter("Command");
		rxParameters.addParameter("Error");
		rxParameters.addParameter("TRACETriggerStatus");

		std::string modifiedTriggerStatus = "";
		std::string xdaqHostname;
		StringMacros::getVectorFromString(hostList, hosts, {';'});
		for(auto& host : hosts)
		{
			// identify artdaq hosts to go through ARTDAQ supervisor
			//	by adding "artdaq.." to hostname artdaq..correlator2.fnal.gov
			__SUP_COUTV__(host);
			if(host.size() < 3)
				continue;  // skip bad hostnames

			// use map to convert to xdaq host
			try
			{
				xdaqHostname = traceMapToXDAQHostname_.at(host);
			}
			catch(...)
			{
				__SUP_SS__ << "Could not find the translation from TRACE hostname '"
				           << host << "' to xdaq Context hostname." << __E__;
				ss << "Here is the existing map (size=" << traceMapToXDAQHostname_.size()
				   << "): " << StringMacros::mapToString(traceMapToXDAQHostname_)
				   << __E__;
				__SUP_SS_THROW__;
			}

			__SUP_COUTV__(xdaqHostname);

			auto& appInfo = allTraceApps.at(xdaqHostname);
			__SUP_COUT__ << "Supervisor hostname = " << host << "/" << xdaqHostname << ":"
			             << appInfo.getId() << " name = " << appInfo.getName()
			             << " class = " << appInfo.getClass()
			             << " hostname = " << appInfo.getHostname() << __E__;
			try
			{
				SOAPParameters txParameters;  // params for xoap to send
				txParameters.addParameter("Request", "SetTriggerEnable");
				txParameters.addParameter("Host", host);

				xoap::MessageReference retMsg = SOAPMessenger::sendWithSOAPReply(
				    appInfo.getDescriptor(), "TRACESupervisorRequest", txParameters);
				SOAPUtilities::receive(retMsg, rxParameters);
				__SUP_COUT__ << "Received TRACE response: "
				             << SOAPUtilities::translate(retMsg).getCommand() << " ==> "
				             << SOAPUtilities::translate(retMsg) << __E__;

				if(SOAPUtilities::translate(retMsg).getCommand() == "Fault")
				{
					__SUP_SS__ << "Unrecognized command at destination TRACE Supervisor "
					              "hostname = "
					           << host << "/" << appInfo.getId()
					           << " name = " << appInfo.getName()
					           << " class = " << appInfo.getClass()
					           << " hostname = " << appInfo.getHostname() << __E__;
					__SUP_SS_THROW__;
				}
				else if(SOAPUtilities::translate(retMsg).getCommand() == "TRACEFault")
				{
					__SUP_SS__ << "Error received: " << rxParameters.getValue("Error")
					           << __E__;
					__SUP_SS_THROW__;
				}
			}
			catch(const xdaq::exception::Exception& e)
			{
				__SUP_SS__ << "Error transmitting request to TRACE Supervisor LID = "
				           << appInfo.getId() << " name = " << appInfo.getName()
				           << ". \n\n"
				           << e.what() << __E__;
				__SUP_SS_THROW__;
			}

			modifiedTriggerStatus += rxParameters.getValue("TRACETriggerStatus");
		}  // end host set TRACE loop

		__SUP_COUT__ << "mod'd TRACE Trigger Status received: \n"
		             << modifiedTriggerStatus << __E__;
		xmlOut.addTextElementToData("modTriggerStatus", modifiedTriggerStatus);
	}  // end SetTriggerEnable
	else if(requestType == "ResetTRACE")
	{
		__SUP_COUT__ << "requestType " << requestType << __E__;

		std::string hostList = CgiDataUtilities::postData(cgiIn, "hostList");

		__SUP_COUTV__(hostList);

		std::vector<std::string /*host*/> hosts;

		auto& allTraceApps = allSupervisorInfo_.getAllTraceControllerSupervisorInfo();

		SOAPParameters rxParameters;  // params for xoap to recv
		rxParameters.addParameter("Command");
		rxParameters.addParameter("Error");
		rxParameters.addParameter("TRACETriggerStatus");

		std::string modifiedTriggerStatus = "";
		std::string xdaqHostname;
		StringMacros::getVectorFromString(hostList, hosts, {';'});
		for(auto& host : hosts)
		{
			// identify artdaq hosts to go through ARTDAQ supervisor
			//	by adding "artdaq.." to hostname artdaq..correlator2.fnal.gov
			__SUP_COUTV__(host);
			if(host.size() < 3)
				continue;  // skip bad hostnames

			// use map to convert to xdaq host
			try
			{
				xdaqHostname = traceMapToXDAQHostname_.at(host);
			}
			catch(...)
			{
				__SUP_SS__ << "Could not find the translation from TRACE hostname '"
				           << host << "' to xdaq Context hostname." << __E__;
				ss << "Here is the existing map (size=" << traceMapToXDAQHostname_.size()
				   << "): " << StringMacros::mapToString(traceMapToXDAQHostname_)
				   << __E__;
				__SUP_SS_THROW__;
			}

			__SUP_COUTV__(xdaqHostname);

			auto& appInfo = allTraceApps.at(xdaqHostname);
			__SUP_COUT__ << "Supervisor hostname = " << host << "/" << xdaqHostname << ":"
			             << appInfo.getId() << " name = " << appInfo.getName()
			             << " class = " << appInfo.getClass()
			             << " hostname = " << appInfo.getHostname() << __E__;
			try
			{
				SOAPParameters txParameters;  // params for xoap to send
				txParameters.addParameter("Request", "ResetTRACE");
				txParameters.addParameter("Host", host);

				xoap::MessageReference retMsg = SOAPMessenger::sendWithSOAPReply(
				    appInfo.getDescriptor(), "TRACESupervisorRequest", txParameters);
				SOAPUtilities::receive(retMsg, rxParameters);
				__SUP_COUT__ << "Received TRACE response: "
				             << SOAPUtilities::translate(retMsg).getCommand() << " ==> "
				             << SOAPUtilities::translate(retMsg) << __E__;

				if(SOAPUtilities::translate(retMsg).getCommand() == "Fault")
				{
					__SUP_SS__ << "Unrecognized command at destination TRACE Supervisor "
					              "hostname = "
					           << host << "/" << appInfo.getId()
					           << " name = " << appInfo.getName()
					           << " class = " << appInfo.getClass()
					           << " hostname = " << appInfo.getHostname() << __E__;
					__SUP_SS_THROW__;
				}
				else if(SOAPUtilities::translate(retMsg).getCommand() == "TRACEFault")
				{
					__SUP_SS__ << "Error received: " << rxParameters.getValue("Error")
					           << __E__;
					__SUP_SS_THROW__;
				}
			}
			catch(const xdaq::exception::Exception& e)
			{
				__SUP_SS__ << "Error transmitting request to TRACE Supervisor LID = "
				           << appInfo.getId() << " name = " << appInfo.getName()
				           << ". \n\n"
				           << e.what() << __E__;
				__SUP_SS_THROW__;
			}

			modifiedTriggerStatus += rxParameters.getValue("TRACETriggerStatus");
		}  // end host set TRACE loop

		__SUP_COUT__ << "mod'd TRACE Trigger Status received: \n"
		             << modifiedTriggerStatus << __E__;
		xmlOut.addTextElementToData("modTriggerStatus", modifiedTriggerStatus);
	}  // end ResetTRACE
	else if(requestType == "EnableTRACE")
	{
		__SUP_COUT__ << "requestType " << requestType << __E__;

		std::string hostList = CgiDataUtilities::postData(cgiIn, "hostList");
		std::string enable   = CgiDataUtilities::postData(cgiIn, "enable");

		__SUP_COUTV__(hostList);
		__SUP_COUTV__(enable);

		std::vector<std::string /*host*/> hosts;

		auto& allTraceApps = allSupervisorInfo_.getAllTraceControllerSupervisorInfo();

		SOAPParameters rxParameters;  // params for xoap to recv
		rxParameters.addParameter("Command");
		rxParameters.addParameter("Error");
		rxParameters.addParameter("TRACETriggerStatus");

		std::string modifiedTriggerStatus = "";
		std::string xdaqHostname;
		StringMacros::getVectorFromString(hostList, hosts, {';'});
		for(auto& host : hosts)
		{
			// identify artdaq hosts to go through ARTDAQ supervisor
			//	by adding "artdaq.." to hostname artdaq..correlator2.fnal.gov
			__SUP_COUTV__(host);
			if(host.size() < 3)
				continue;  // skip bad hostnames

			// use map to convert to xdaq host
			try
			{
				xdaqHostname = traceMapToXDAQHostname_.at(host);
			}
			catch(...)
			{
				__SUP_SS__ << "Could not find the translation from TRACE hostname '"
				           << host << "' to xdaq Context hostname." << __E__;
				ss << "Here is the existing map (size=" << traceMapToXDAQHostname_.size()
				   << "): " << StringMacros::mapToString(traceMapToXDAQHostname_)
				   << __E__;
				__SUP_SS_THROW__;
			}

			__SUP_COUTV__(xdaqHostname);

			auto& appInfo = allTraceApps.at(xdaqHostname);
			__SUP_COUT__ << "Supervisor hostname = " << host << "/" << xdaqHostname << ":"
			             << appInfo.getId() << " name = " << appInfo.getName()
			             << " class = " << appInfo.getClass()
			             << " hostname = " << appInfo.getHostname() << __E__;
			try
			{
				SOAPParameters txParameters;  // params for xoap to send
				txParameters.addParameter("Request", "EnableTRACE");
				txParameters.addParameter("Host", host);
				txParameters.addParameter("SetEnable", enable);

				xoap::MessageReference retMsg = SOAPMessenger::sendWithSOAPReply(
				    appInfo.getDescriptor(), "TRACESupervisorRequest", txParameters);
				SOAPUtilities::receive(retMsg, rxParameters);
				__SUP_COUT__ << "Received TRACE response: "
				             << SOAPUtilities::translate(retMsg).getCommand() << " ==> "
				             << SOAPUtilities::translate(retMsg) << __E__;

				if(SOAPUtilities::translate(retMsg).getCommand() == "Fault")
				{
					__SUP_SS__ << "Unrecognized command at destination TRACE Supervisor "
					              "hostname = "
					           << host << "/" << appInfo.getId()
					           << " name = " << appInfo.getName()
					           << " class = " << appInfo.getClass()
					           << " hostname = " << appInfo.getHostname() << __E__;
					__SUP_SS_THROW__;
				}
				else if(SOAPUtilities::translate(retMsg).getCommand() == "TRACEFault")
				{
					__SUP_SS__ << "Error received: " << rxParameters.getValue("Error")
					           << __E__;
					__SUP_SS_THROW__;
				}
			}
			catch(const xdaq::exception::Exception& e)
			{
				__SUP_SS__ << "Error transmitting request to TRACE Supervisor LID = "
				           << appInfo.getId() << " name = " << appInfo.getName()
				           << ". \n\n"
				           << e.what() << __E__;
				__SUP_SS_THROW__;
			}

			modifiedTriggerStatus += rxParameters.getValue("TRACETriggerStatus");
		}  // end host set TRACE loop

		__SUP_COUT__ << "mod'd TRACE Trigger Status received: \n"
		             << modifiedTriggerStatus << __E__;
		xmlOut.addTextElementToData("modTriggerStatus", modifiedTriggerStatus);
	}  // end EnableTRACE
	else if(requestType == "GetTraceSnapshot")
	{
		__SUP_COUT__ << "requestType " << requestType << __E__;

		std::string hostList  = CgiDataUtilities::postData(cgiIn, "hostList");
		std::string filterFor = CgiDataUtilities::postData(cgiIn, "filterFor");
		std::string filterOut = CgiDataUtilities::postData(cgiIn, "filterOut");

		__SUP_COUTV__(hostList);
		__SUP_COUTV__(filterFor);
		__SUP_COUTV__(filterOut);

		std::vector<std::string /*host*/> hosts;

		auto& allTraceApps = allSupervisorInfo_.getAllTraceControllerSupervisorInfo();

		SOAPParameters rxParameters;  // params for xoap to recv
		rxParameters.addParameter("Command");
		rxParameters.addParameter("Error");
		rxParameters.addParameter("TRACETriggerStatus");
		rxParameters.addParameter("TRACESnapshot");

		std::string modifiedTriggerStatus = "";
		std::string xdaqHostname;
		StringMacros::getVectorFromString(hostList, hosts, {';'});
		for(auto& host : hosts)
		{
			// identify artdaq hosts to go through ARTDAQ supervisor
			//	by adding "artdaq.." to hostname artdaq..correlator2.fnal.gov
			__SUP_COUTV__(host);
			if(host.size() < 3)
				continue;  // skip bad hostnames

			// use map to convert to xdaq host
			try
			{
				xdaqHostname = traceMapToXDAQHostname_.at(host);
			}
			catch(...)
			{
				__SUP_SS__ << "Could not find the translation from TRACE hostname '"
				           << host << "' to xdaq Context hostname." << __E__;
				ss << "Here is the existing map (size=" << traceMapToXDAQHostname_.size()
				   << "): " << StringMacros::mapToString(traceMapToXDAQHostname_)
				   << __E__;
				__SUP_SS_THROW__;
			}

			__SUP_COUTV__(xdaqHostname);

			auto& appInfo = allTraceApps.at(xdaqHostname);
			__SUP_COUT__ << "Supervisor hostname = " << host << "/" << xdaqHostname << ":"
			             << appInfo.getId() << " name = " << appInfo.getName()
			             << " class = " << appInfo.getClass()
			             << " hostname = " << appInfo.getHostname() << __E__;
			try
			{
				SOAPParameters txParameters;  // params for xoap to send
				txParameters.addParameter("Request", "GetSnapshot");
				txParameters.addParameter("Host", host);
				txParameters.addParameter("FilterForCSV", filterFor);
				txParameters.addParameter("FilterOutCSV", filterOut);

				xoap::MessageReference retMsg = SOAPMessenger::sendWithSOAPReply(
				    appInfo.getDescriptor(), "TRACESupervisorRequest", txParameters);
				SOAPUtilities::receive(retMsg, rxParameters);
				__SUP_COUT__ << "Received TRACE response: "
				             << SOAPUtilities::translate(retMsg).getCommand() << __E__;
				//<< " ==> Bytes " << SOAPUtilities::translate(retMsg) << __E__;

				if(SOAPUtilities::translate(retMsg).getCommand() == "Fault")
				{
					__SUP_SS__ << "Unrecognized command at destination TRACE Supervisor "
					              "hostname = "
					           << host << "/" << appInfo.getId()
					           << " name = " << appInfo.getName()
					           << " class = " << appInfo.getClass()
					           << " hostname = " << appInfo.getHostname() << __E__;
					__SUP_SS_THROW__;
				}
				else if(SOAPUtilities::translate(retMsg).getCommand() == "TRACEFault")
				{
					__SUP_SS__ << "Error received: " << rxParameters.getValue("Error")
					           << __E__;
					__SUP_SS_THROW__;
				}
			}
			catch(const xdaq::exception::Exception& e)
			{
				__SUP_SS__ << "Error transmitting request to TRACE Supervisor LID = "
				           << appInfo.getId() << " name = " << appInfo.getName()
				           << ". \n\n"
				           << e.what() << __E__;
				__SUP_SS_THROW__;
			}

			modifiedTriggerStatus += rxParameters.getValue("TRACETriggerStatus");
			xmlOut.addTextElementToData("host", host);
			std::string snapshot = rxParameters.getValue("TRACESnapshot");
			//			if(snapshot.size() > 100000)
			//			{
			//				__SUP_COUT__ << "Truncating snapshot" << __E__;
			//				snapshot.resize(100000);
			//			}
			//			xmlOut.addTextElementToData("hostSnapshot", snapshot);

			{
				std::string filename =
				    USER_CONSOLE_SNAPSHOT_PATH + "snapshot_" + host + ".txt";
				__SUP_COUTV__(filename);
				FILE* fp = fopen(filename.c_str(), "w");
				if(!fp)
				{
					__SUP_SS__ << "Failed to create snapshot file: " << filename << __E__;
					__SUP_SS_THROW__;
				}
				fprintf(fp,
				        "TRACE Snapshot taken at %s\n",
				        StringMacros::getTimestampString().c_str());

				if(snapshot.size() > 5 && snapshot[2] != 'i')
				{
					// add header lines
					fprintf(
					    fp,
					    "  idx           us_tod       delta    pid    tid cpu            "
					    "                    trcname lvl r msg                       \n");
					fprintf(fp,
					        "----- ---------------- ----------- ------ ------ --- "
					        "-------------------------------------- --- - "
					        "--------------------------\n");
				}
				fprintf(fp, "%s", snapshot.c_str());
				fclose(fp);
			}
		}  // end host set TRACE loop

		__SUP_COUT__ << "mod'd TRACE Trigger Status received: \n"
		             << modifiedTriggerStatus << __E__;
		xmlOut.addTextElementToData("modTriggerStatus", modifiedTriggerStatus);
	}  // end getTraceSnapshot
	else if(requestType == "GetCustomCountsAndActions" ||
	        requestType == "AddCustomCountsAndAction" ||
	        requestType == "ModifyCustomCountsAndAction")
	{
		__SUP_COUT__ << "requestType " << requestType
		             << " size=" << priorityCustomTriggerList_.size() << __E__;

		//mutex scope:
		// lockout the messages array for the remainder of the scope
		// this guarantees can safely access the action queue
		std::lock_guard<std::mutex> lock(messageMutex_);

		if(requestType == "AddCustomCountsAndAction" ||
		   requestType == "ModifyCustomCountsAndAction")
		{
			std::string needle = StringMacros::decodeURIComponent(
			    CgiDataUtilities::postData(cgiIn, "needle"));
			uint32_t    priority = CgiDataUtilities::postDataAsInt(cgiIn, "priority");
			std::string action   = StringMacros::decodeURIComponent(
                CgiDataUtilities::postData(cgiIn, "action"));
			uint32_t triggerOnCount = CgiDataUtilities::postDataAsInt( cgiIn, "triggerOnCount");
			bool doLoop = CgiDataUtilities::postDataAsInt( cgiIn, "doLoop") > 0 ? true : false;

			__SUP_COUTV__(needle);
			__SUP_COUTV__(priority);
			__SUP_COUTV__(action);
			__SUP_COUTV__(triggerOnCount);
			__SUP_COUTV__(doLoop);

			// TODO: Modify triggerOnCount and doLoop
			if(requestType == "ModifyCustomCountsAndAction")
			{
				std::string buttonDo = StringMacros::decodeURIComponent(
				    CgiDataUtilities::postData(cgiIn, "buttonDo"));
				std::string currentNeedle =
				    CgiDataUtilities::postData(cgiIn, "currentNeedle");

				//treat needle as CSV list and do in reverse order to maintain priority of group
				std::vector<std::string> csvNeedles =
				    StringMacros::getVectorFromString(currentNeedle, {','});
				for(size_t i = csvNeedles.size() - 1; i < csvNeedles.size(); --i)
				{
					if(csvNeedles[i].size() == 0)
						continue;  //skip empty entries
					//change the priority to the last placed priority entry to keep group order
					priority = modifyCustomTriggeredAction(
					    StringMacros::decodeURIComponent(csvNeedles[i]),
					    buttonDo,
					    needle,
					    action,
					    priority,
						triggerOnCount,
						doLoop);
				}  //end csv needle list handling
			}
			else
				addCustomTriggeredAction(needle, action, priority);

			saveCustomCountList();
		}  // end AddCustomCountsAndAction

		//always calculate untriggered count
		size_t untriggeredCount =
		    messageCount_;  //copy and then decrement "unique" incrementing ID for messages

		for(const auto& customCount : priorityCustomTriggerList_)
		{
			xercesc::DOMElement* customCountParent =
			    xmlOut.addTextElementToData("customCount", "");

			if(customCount.occurrences < untriggeredCount)
				untriggeredCount -= customCount.occurrences;
			else
			{
				__SUP_SS__ << "Impossible custom count; notify admins! "
				           << customCount.occurrences << " > " << untriggeredCount
				           << " for "
				           << StringMacros::vectorToString(customCount.needleSubstrings,
				                                           {'*'})
				           << __E__;
				__SUP_SS_THROW__;
			}
			xmlOut.addTextElementToParent(
			    "needle",
			    StringMacros::vectorToString(customCount.needleSubstrings, {'*'}),
			    customCountParent);
			xmlOut.addTextElementToParent(
			    "count", std::to_string(customCount.occurrences), customCountParent);
			xmlOut.addTextElementToParent(
			    "action", customCount.action, customCountParent);
		}  //end adding custom counts to response xml loop

		//add untriggered always last
		xercesc::DOMElement* customCountParent =
		    xmlOut.addTextElementToData("customCount", "");
		xmlOut.addTextElementToParent("needle", "< Untriggered >", customCountParent);
		xmlOut.addTextElementToParent(
		    "count", std::to_string(untriggeredCount), customCountParent);
		xmlOut.addTextElementToParent("action", "Count Only", customCountParent);

	}  // end GetCustomCountsAndActions or AddCustomCountsAndAction
	else
	{
		__SUP_SS__ << "requestType Request, " << requestType << ", not recognized."
		           << __E__;
		__SUP_SS_THROW__;
	}
}  // end request()

//==============================================================================
/// virtual progress string that can be overridden with more info
///	like Console Error and Warning count
std::string ConsoleSupervisor::getStatusProgressDetail(void)
{
	//Console Supervisor status detatil format is:
	//	uptime, Err count, Warn count, Last Error msg, Last Warn msg

	//return uptime detail
	std::stringstream ss;
	ss << "Uptime: "
	   << StringMacros::encodeURIComponent(StringMacros::getTimeDurationString(
	          CorePropertySupervisorBase::getSupervisorUptime()));

	//return Err count, Warn count, Last Error msg, Last Warn msg, Last Info msg, Info count

	// size_t 			errorCount_ = 0, warnCount_ = 0;
	// std::string 	lastErrorMessage_, lastWarnMessage_;
	// time_t			lastErrorMessageTime_ = 0, lastWarnMessageTime_ = 0;

	ss << ", Error #: " << errorCount_;
	ss << ", Warn #: " << warnCount_;
	ss << ", Last Error ("
	   << (lastErrorMessageTime_ ? StringMacros::getTimestampString(lastErrorMessageTime_)
	                             : "0")
	   << "): "
	   << (lastErrorMessageTime_ ? StringMacros::encodeURIComponent(lastErrorMessage_)
	                             : "");
	ss << ", Last Warn ("
	   << (lastWarnMessageTime_ ? StringMacros::getTimestampString(lastWarnMessageTime_)
	                            : "0")
	   << "): "
	   << (lastWarnMessageTime_ ? StringMacros::encodeURIComponent(lastWarnMessage_)
	                            : "");
	ss << ", Last Info ("
	   << (lastInfoMessageTime_ ? StringMacros::getTimestampString(lastInfoMessageTime_)
	                            : "0")
	   << "): "
	   << (lastInfoMessageTime_ ? StringMacros::encodeURIComponent(lastInfoMessage_)
	                            : "");
	ss << ", Info #: " << infoCount_;
	ss << ", First Error ("
	   << (firstErrorMessageTime_
	           ? StringMacros::getTimestampString(firstErrorMessageTime_)
	           : "0")
	   << "): "
	   << (firstErrorMessageTime_ ? StringMacros::encodeURIComponent(firstErrorMessage_)
	                              : "");
	ss << ", First Warn ("
	   << (firstWarnMessageTime_ ? StringMacros::getTimestampString(firstWarnMessageTime_)
	                             : "0")
	   << "): "
	   << (firstWarnMessageTime_ ? StringMacros::encodeURIComponent(firstWarnMessage_)
	                             : "");
	ss << ", First Info ("
	   << (firstInfoMessageTime_ ? StringMacros::getTimestampString(firstInfoMessageTime_)
	                             : "0")
	   << "): "
	   << (firstInfoMessageTime_ ? StringMacros::encodeURIComponent(firstInfoMessage_)
	                             : "");

	return ss.str();
}  // end getStatusProgressDetail()

//==============================================================================
/// ConsoleSupervisor::insertMessageRefresh()
///	if lastUpdateClock is current, return nothing
///	else return new messages
///	(note: lastUpdateIndex==(unsigned int)-1 first time and returns as much as possible//
/// nothing but lastUpdateClock)
///
///	format of xml:
///
///	<last_update_count/>
///	<last_update_index/>
///	<messages>
///		<message_FIELDNAME*/>
///"Level"
///"Label"
///"Source"
///"Msg"
///"Time"
///"Count"
///	</messages>
///
///	NOTE: Uses std::mutex to avoid conflict with writing thread. (this is the reading
/// thread)
void ConsoleSupervisor::insertMessageRefresh(HttpXmlDocument* xmlOut,
                                             const size_t     lastUpdateCount)
{
	//__SUP_COUT__ << __E__;

	if(messages_.size() == 0)
		return;

	// validate lastUpdateCount
	if(lastUpdateCount > messages_.back().getCount() && lastUpdateCount != (size_t)-1)
	{
		__SS__ << "Invalid lastUpdateCount: " << lastUpdateCount
		       << " messagesArray size = " << messages_.back().getCount() << __E__;
		__SS_THROW__;
	}

	// lockout the messages array for the remainder of the scope
	// this guarantees the reading thread can safely access the messages
	std::lock_guard<std::mutex> lock(messageMutex_);

	xmlOut->addTextElementToData("last_update_count",
	                             std::to_string(messages_.back().getCount()));

	refreshParent_ = xmlOut->addTextElementToData("messages", "");

	bool        requestOutOfSync = false;
	std::string requestOutOfSyncMsg;

	size_t refreshReadPointer = 0;
	if(lastUpdateCount != (size_t)-1)
	{
		while(refreshReadPointer < messages_.size() &&
		      messages_[refreshReadPointer].getCount() <= lastUpdateCount)
		{
			++refreshReadPointer;
		}
	}

	if(refreshReadPointer >= messages_.size())
		return;

	// limit number of catch-up messages
	if(messages_.size() - refreshReadPointer > maxClientMessageRequest_)
	{
		// __SUP_COUT__ << "Only sending latest " << maxClientMessageRequest_ << "
		// messages!";

		// auto oldrrp        = refreshReadPointer;
		refreshReadPointer = messages_.size() - maxClientMessageRequest_;

		// __SS__ << "Skipping " << (refreshReadPointer - oldrrp)
		//        << " messages because the web console has fallen behind!" << __E__;
		// __COUT__ << ss.str();
		// ConsoleMessageStruct msg(CONSOLE_SPECIAL_WARNING + ss.str(), lastUpdateCount);
		// auto                 it = messages_.begin();
		// std::advance(it, refreshReadPointer + 1);
		// messages_.insert(it, msg);
	}

	//return first_update_count, so that older messages could be retrieved later if desired by user
	xmlOut->addTextElementToData(
	    "earliest_update_count",
	    std::to_string(messages_[refreshReadPointer].getCount()));

	// output oldest to new
	for(; refreshReadPointer < messages_.size(); ++refreshReadPointer)
	{
		auto msg = messages_[refreshReadPointer];
		if(msg.getCount() < lastUpdateCount)
		{
			if(!requestOutOfSync)  // record out of sync message once only
			{
				requestOutOfSync = true;
				__SS__ << "Request is out of sync! Message count should be more recent! "
				       << msg.getCount() << " < " << lastUpdateCount << __E__;
				requestOutOfSyncMsg = ss.str();
			}
			// assume these messages are new (due to a system restart)
			// continue;
		}

		addMessageToResponse(xmlOut, msg);

	}  //end main message add loop

	if(requestOutOfSync)  // if request was out of sync, show message
		__SUP_COUT__ << requestOutOfSyncMsg;
}  // end insertMessageRefresh()

//==============================================================================
/// ConsoleSupervisor::prependHistoricMessages()
///	if earliestOnhandMessageCount is 0, return nothing
///	else return earlier messages as much as possible
///
///	format of xml:
///
///	<last_update_count/>
///	<last_update_index/>
///	<messages>
///		<message_FIELDNAME*/>
///"Level"
///"Label"
///"Source"
///"Msg"
///"Time"
///"Count"
///	</messages>
///
///	NOTE: Uses std::mutex to avoid conflict with writing thread. (this is the reading
/// thread)
void ConsoleSupervisor::prependHistoricMessages(HttpXmlDocument* xmlOut,
                                                const size_t earliestOnhandMessageCount)
{
	//__SUP_COUT__ << __E__;

	if(messages_.size() == 0)
		return;

	// validate earliestOnhandMessageCount
	if(earliestOnhandMessageCount >= messages_.back().getCount())
	{
		__SS__
		    << "Invalid claim from user request of earliest onhand message sequence ID = "
		    << earliestOnhandMessageCount
		    << ". Latest existing sequence ID = " << messages_.back().getCount()
		    << ". Was the Console Supervisor restarted?" << __E__;
		__SS_THROW__;
	}

	// lockout the messages array for the remainder of the scope
	// this guarantees the reading thread can safely access the messages
	std::lock_guard<std::mutex> lock(messageMutex_);

	refreshParent_ = xmlOut->addTextElementToData("messages", "");

	size_t refreshReadPointer = 0;
	size_t readCountStart     = earliestOnhandMessageCount - maxClientMessageRequest_;
	if(readCountStart >= messages_.back().getCount())  //then wrapped around, so set to 0
		readCountStart = 0;

	//find starting read pointer
	while(refreshReadPointer < messages_.size() &&
	      messages_[refreshReadPointer].getCount() < readCountStart)
	{
		++refreshReadPointer;
	}

	if(refreshReadPointer >= messages_.size())
		return;

	xmlOut->addTextElementToData("earliest_update_count",  //return new early onhand count
	                             std::to_string(readCountStart));

	//messages returned will be from readCountStart to earliestOnhandMessageCount-1
	// output oldest to new
	for(; refreshReadPointer < messages_.size(); ++refreshReadPointer)
	{
		auto msg = messages_[refreshReadPointer];
		if(messages_[refreshReadPointer].getCount() >= earliestOnhandMessageCount)
			break;  //found last message

		addMessageToResponse(xmlOut, msg);

	}  //end main message add loop

}  // end prependHistoricMessages()

//==============================================================================
void ConsoleSupervisor::addMessageToResponse(HttpXmlDocument* xmlOut,
                                             ConsoleSupervisor::ConsoleMessageStruct& msg)
{
	// for all fields, give value
	for(auto& field : msg.fields)
	{
		if(field.first == ConsoleMessageStruct::FieldType::SOURCE)
			continue;  // skip, not userful
		if(field.first == ConsoleMessageStruct::FieldType::SOURCEID)
			continue;  // skip, not userful
		if(field.first == ConsoleMessageStruct::FieldType::SEQID)
			continue;  // skip, not userful
		if(field.first == ConsoleMessageStruct::FieldType::TIMESTAMP)  //use Time instead
			continue;  // skip, not userful
		if(field.first ==
		   ConsoleMessageStruct::FieldType::LEVEL)  //use modified getLevel instead
			continue;                               // skip, not userful

		xmlOut->addTextElementToParent(
		    "message_" + ConsoleMessageStruct::fieldNames.at(field.first),
		    field.second,
		    refreshParent_);
	}  //end msg field loop

	// give modified level also
	xmlOut->addTextElementToParent("message_Level", msg.getLevel(), refreshParent_);

	// give timestamp also
	xmlOut->addTextElementToParent("message_Time", msg.getTime(), refreshParent_);

	// give global count index also
	xmlOut->addTextElementToParent(
	    "message_Count", std::to_string(msg.getCount()), refreshParent_);

	//give Custom count label also (i.e., which search string this message matches, or blank "" for no match)
	xmlOut->addTextElementToParent(
	    "message_Custom",
	    StringMacros::vectorToString(msg.getCustomTriggerMatch().needleSubstrings, {'*'}),
	    refreshParent_);

}  // end addMessageToResponse()
