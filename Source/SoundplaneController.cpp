
// Part of the Soundplane client software by Madrona Labs.
// Copyright (c) 2013 Madrona Labs LLC. http://www.madronalabs.com
// Distributed under the MIT license: http://madrona-labs.mit-license.org/

#include "SoundplaneController.h"
	
const char *kUDPType      =   "_osc._udp";
const char *kLocalDotDomain   =   "local.";

SoundplaneController::SoundplaneController(SoundplaneModel* pModel) :
	MLReporter(pModel),
	mpSoundplaneModel(pModel),
	mpSoundplaneView(0),
	mCurrMenuInstigator(nullptr)
{
	MLReporter::mpModel = pModel;
	initialize();
	startTimer(250);
}

SoundplaneController::~SoundplaneController()
{
}
	
static const std::string kOSCDefaultStr("localhost:3123 (default)");

void SoundplaneController::initialize()
{
	// prime MIDI device pump
	StringArray devices;
	devices = MidiOutput::getDevices();
	
	// make OSC services list
	mOSCServicesMenu.clear();
	mServiceNames.clear();
	services.clear();
	services.push_back(kOSCDefaultStr);
	Browse(kUDPType, kLocalDotDomain);
}
	
void SoundplaneController::shutdown()
{
	
}

void SoundplaneController::timerCallback()
{
	updateChangedParams();
	PollNetServices();
	debug().display();
}
	
void SoundplaneController::buttonClicked (MLButton* pButton)
{
	MLSymbol p (pButton->getParamName());
	MLParamValue t = pButton->getToggleState();

	mpModel->setModelParam(p, t);
	
	/*
	if (p == "carriers")
	{
		MLParamValue b = pButton->getToggleState();
		debug() << "buttonClicked: " << b << "\n";
        mpModel->enableCarriers(b ? 0xFFFFFFFF : 0); 
	}
	*/
	if (p == "clear")
	{
		mpSoundplaneModel->clear();
	}
	else if (p == "select_carriers")
	{		
		mpSoundplaneModel->beginSelectCarriers();
	}
	else if (p == "default_carriers")
	{		
		mpSoundplaneModel->setDefaultCarriers();
	}
	else if (p == "calibrate")
	{
		mpSoundplaneModel->beginCalibrate();
	}
	else if (p == "preset")
	{
	
	}
	else if (p == "calibrate_tracker")
	{
		mpSoundplaneModel->beginCalibrateTracker();
		if(mpSoundplaneView)
		{
			MLWidget* pB = mpSoundplaneView->getWidget("calibrate_tracker_cancel");
			pB->getComponent()->setEnabled(true);
		}
	}
	else if (p == "calibrate_tracker_cancel")
	{
		mpSoundplaneModel->cancelCalibrateTracker();
	}
}


void SoundplaneController::dialValueChanged (MLDial* pDial)
{
	if (!pDial) return;
	
	// mpModel->setParameter(pDial->getParamName(), pDial->getValue());
	
	MLSymbol p (pDial->getParamName());
	MLParamValue v = pDial->getValue();

	debug() << p << ": " << v << "\n";
	
	mpModel->setModelParam(p, v);
	
}

void SoundplaneController::setView(SoundplaneView* v) 
{ 
	mpSoundplaneView = v; 
}

void SoundplaneController::setupMenus()
{
	if(!mpSoundplaneView) return;
	
	MLMenuPtr viewMenu = MLMenuPtr(new MLMenu());
	mMenuMap["viewmode"] = viewMenu;
	viewMenu->addItem("raw data");
	viewMenu->addItem("calibrated");
	viewMenu->addItem("cooked");
	viewMenu->addItem("xy");
	viewMenu->addItem("test");
	
	// collect MIDI menu each time
	MLMenuPtr midiMenu = MLMenuPtr(new MLMenu());
	mMenuMap["midi_device"] = midiMenu;
		
	// presets each time
	MLMenuPtr presetMenu = MLMenuPtr(new MLMenu());
	mMenuMap["preset"] = presetMenu;
	presetMenu->addItem("continuous pitch x");
	presetMenu->addItem("rows in fourths");
	
	MLMenuPtr oscMenu = MLMenuPtr(new MLMenu());
	mMenuMap["osc_services"] = oscMenu;
	
	// setup defaults 
	mpModel->setModelParam("osc_services", kOSCDefaultStr);	
}	

void SoundplaneController::menuItemChosen(MLSymbol menuName, int result)
{
	SoundplaneModel* pModel = getModel();
	assert(pModel);

	if (result > 0)
	{
		int menuIdx = result - 1;
		MLMenuPtr menu = mMenuMap[menuName];
		if (menu != MLMenuPtr())
		{
			pModel->setModelParam(menuName, menu->getItemString(menuIdx));
		}

		if (menuName == "osc_services")
		{
			std::string name;
			if(result == 1) // set default
			{
				name = "default";
				SoundplaneOSCOutput& output = pModel->getOSCOutput();
				output.connect(kDefaultHostnameString, kDefaultUDPPort);
				pModel->setKymaMode(0);
			}
			else // resolve a service from list
			{
				name = getServiceName(menuIdx);
				Resolve(name.c_str(), kUDPType, kLocalDotDomain);
			}			
		}
	}
}

static void menuItemChosenCallback (int result, SoundplaneController* pC, MLSymbol menuName);
static void menuItemChosenCallback (int result, SoundplaneController* pC, MLSymbol menuName)
{
	MLMenuButton* instigator = pC->getCurrMenuInstigator();
	if(instigator)
	{
		instigator->setToggleState(false, false);
	}
	pC->menuItemChosen(menuName, result);
}

void SoundplaneController::showMenu (MLSymbol menuName, MLMenuButton* instigator)
{
	StringArray devices;
	if(!mpSoundplaneView) return;
	
	// handle possible click on second menu while first is active
	if(getCurrMenuInstigator() != nullptr)
	{
		getCurrMenuInstigator()->setToggleState(false, false);
	}
	
	MLMenuPtr menu = mMenuMap[menuName];
	mCurrMenuName = menuName;
	assert(instigator);
	setCurrMenuInstigator(instigator);
	instigator->setToggleState(true, false);

	MLLookAndFeel* myLookAndFeel = MLLookAndFeel::getInstance(); // should get from View
	int u = myLookAndFeel->getGridUnitSize();
	int height = ((float)u)*0.35f;
	height = clamp(height, 12, 128);
	
	// update menus that might change each time
	if (menuName == "preset")
	{
		// populate preset menu
		// TODO look at files in preset dir
		//
		menu->clear();
		menu->addItem("continuous pitch x");
		menu->addItem("rows in fourths");
	}
	else if (menuName == "midi_device")
	{
		// refresh device list
		menu->clear();		
		SoundplaneMIDIOutput& outs = getModel()->getMIDIOutput();
		outs.findMIDIDevices ();
		std::list<std::string>& devices = outs.getDeviceList();
		menu->addItems(devices);
	}
	else if (menuName == "osc_services")
	{
		menu->clear();		
		mServiceNames.clear();
		std::vector<std::string>::iterator it;
		for(it = services.begin(); it != services.end(); it++)
		{
			const std::string& serviceName = *it;
			std::string formattedName;
			formatServiceName(serviceName, formattedName);
			mServiceNames.push_back(serviceName);
			menu->addItem(formattedName);
		}
	}	
	
	if(menu != MLMenuPtr())
	{
		PopupMenu& juceMenu = menu->getJuceMenu();
		juceMenu.showMenuAsync (PopupMenu::Options().withTargetComponent(instigator).withStandardItemHeight(height),
			ModalCallbackFunction::withParam(menuItemChosenCallback, this, menuName));
	}
}

void SoundplaneController::formatServiceName(const std::string& inName, std::string& outName)
{
	const char* inStr = inName.c_str();
	if(!strncmp(inStr, "beslime", 7))
	{
		outName = inName + std::string(" (Kyma)");
	}
	else
	{
		outName = inName;
	}
}

const std::string& SoundplaneController::getServiceName(int idx)
{
	return mServiceNames[idx];
}

// called asynchronously after Resolve() when host and port are found
//
void SoundplaneController::didResolveAddress(NetService *pNetService)
{
	const std::string& serviceName = pNetService->getName();
	const std::string& hostName = pNetService->getHostName();
	const char* hostNameStr = hostName.c_str();
	int port = pNetService->getPort();
	
	debug() << "resolved net service to " << hostName << ", " << port << "\n";
	
	// TEMP todo don't access output directly
	if(mpSoundplaneModel)
	{
		SoundplaneOSCOutput& output = mpSoundplaneModel->getOSCOutput();
		output.connect(hostNameStr, port);
	}
	
	// if we are talking to a kyma, set kyma mode
	static const char* kymaStr = "beslime";
	int len = strlen(kymaStr);
	bool isProbablyKyma = !strncmp(serviceName.c_str(), kymaStr, len);
debug() << "kyma mode " << isProbablyKyma << "\n";
	mpSoundplaneModel->setKymaMode(isProbablyKyma);
	
}


/*
void SoundplaneController::doMidiMenuItem(int result)
{

debug() << "MIDI channel :"	<< result << "\n";

//	if(pTrigButton)
//		pTrigButton->setToggleState(false, false);
}


*/

