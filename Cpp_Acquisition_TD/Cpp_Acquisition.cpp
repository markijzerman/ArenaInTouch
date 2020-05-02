/***************************************************************************************
 ***                                                                                 ***
 ***  Copyright (c) 2019, Lucid Vision Labs, Inc.                                    ***
 ***                                                                                 ***
 ***  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR     ***
 ***  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,       ***
 ***  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE    ***
 ***  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER         ***
 ***  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,  ***
 ***  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE  ***
 ***  SOFTWARE.                                                                      ***
 ***                                                                                 ***
 ***************************************************************************************/

#include "Cpp_Acquisition.h"
#include "ArenaApi.h"
#include "SaveApi.h"
#include "stdafx.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <cmath>
#include <random>
#include <chrono>

 // Uncomment this if you want to run an example that fills the data using threading
 //#define THREADING_EXAMPLE

 // The threading example can run in two modes. One where the producer is continually
 // producing new frames that the consumer (main thread) picks up when able.
 // This more is useful for things such as external device input. This is the default
 // mode.

 // The second mode can be instead used by defining THREADING_SINGLED_PRODUCER.
 // (I.e Uncommenting the below line)
 // In this mode the main thread will signal to the producer thread to generate a new
 // frame each time it consumes a frame.
 // Assuming the producer will generate a new frame in time before execute() gets called
 // again this gives a better 1:1 sync between producing and consuming frames.

 //#define THREADING_SIGNALED_PRODUCER

 // These functions are basic C function, which the DLL loader can find
 // much easier than finding a C++ Class.
 // The DLLEXPORT prefix is needed so the compile exports these functions from the .dll
 // you are creating
extern "C"
{

DLLEXPORT
void 
FillTOPPluginInfo(TOP_PluginInfo* info)
{
		// This must always be set to this constant
		info->apiVersion = TOPCPlusPlusAPIVersion;

		// Change this to change the executeMode behavior of this plugin.
		info->executeMode = TOP_ExecuteMode::CPUMemWriteOnly;

		// The opType is the unique name for this TOP. It must start with a 
		// capital A-Z character, and all the following characters must lower case
		// or numbers (a-z, 0-9)
		info->customOPInfo.opType->setString("Cpumemsample");

		// The opLabel is the text that will show up in the OP Create Dialog
		info->customOPInfo.opLabel->setString("CPU Mem Sample");

		// Will be turned into a 3 letter icon on the nodes
		info->customOPInfo.opIcon->setString("CPM");

		// Information about the author of this OP
		info->customOPInfo.authorName->setString("Author Name");
		info->customOPInfo.authorEmail->setString("email@email.com");

		// This TOP works with 0 or 1 inputs connected
		info->customOPInfo.minInputs = 0;
		info->customOPInfo.maxInputs = 1;
	}

	DLLEXPORT
		TOP_CPlusPlusBase*
		CreateTOPInstance(const OP_NodeInfo* info, TOP_Context* context)
	{
		// Return a new instance of your class every time this is called.
		// It will be called once per TOP that is using the .dll
		return new Cpp_Acquisition(info);
	}

	DLLEXPORT
		void
		DestroyTOPInstance(TOP_CPlusPlusBase* instance, TOP_Context* context)
	{
		// Delete the instance here, this will be called when
		// Touch is shutting down, when the TOP using that instance is deleted, or
		// if the TOP loads a different DLL
		delete (Cpp_Acquisition*)instance;
	}

};



Cpp_Acquisition::Cpp_Acquisition(const OP_NodeInfo* info) :
	myNodeInfo(info),
	myThread(nullptr),
	myThreadShouldExit(false),
	myStartWork(false)
{
	myExecuteCount = 0;
	myStep = 0.0;

	std::cout << "Hi Touch\n";

	pSystem = Arena::OpenSystem();
	std::cout << pSystem->GetTLSystemNodeMap() << "\n";
	pSystem->UpdateDevices(100);
	std::vector<Arena::DeviceInfo> deviceInfos = pSystem->GetDevices();

	numDevices = deviceInfos.size();
	if (numDevices > 0)
	{
		std::cout << "We have " << numDevices << " device\n";
		pDevice = pSystem->CreateDevice(deviceInfos[0]);
		pDevice->StartStream();

		GenApi::INodeMap* pNodeMap = pDevice->GetNodeMap();
		coordinateScale = static_cast<float>(Arena::GetNodeValue<double>(pNodeMap, "Scan3dCoordinateScale"));

		std::cout << "Stream started\n";
	}
	else {
		std::cout << "We dont have a device\n";
	}

}

Cpp_Acquisition::~Cpp_Acquisition()
{

	if (myThread)
	{
		myThreadShouldExit.store(true);
		// Incase the thread is sleeping waiting for a signal
		// to create more work, wake it up
		startMoreWork();
		if (myThread->joinable())
		{
			myThread->join();
		}
		delete myThread;
	}

	std::cout << "Stopping stream\n";
	pDevice->StopStream();
	std::cout << "Destroying device\n";
	pSystem->DestroyDevice(pDevice);
	std::cout << "Closing the system\n";
	Arena::CloseSystem(pSystem);
	std::cout << "--- Houdoe!!\n";
}

void
Cpp_Acquisition::getGeneralInfo(TOP_GeneralInfo* ginfo, const OP_Inputs* inputs, void* reserved1)
{
	ginfo->cookEveryFrame = true;
	ginfo->memPixelType = OP_CPUMemPixelType::RGBA32Float;
}

bool
Cpp_Acquisition::getOutputFormat(TOP_OutputFormat* format, const OP_Inputs* inputs, void* reserved1)
{
	// In this function we could assign variable values to 'format' to specify
	// the pixel format/resolution etc that we want to output to.
	// If we did that, we'd want to return true to tell the TOP to use the settings we've
	// specified.
	// In this example we'll return false and use the TOP's settings
	format->width = 640;
	format->height = 480;
	return format;
}


void
Cpp_Acquisition::execute(TOP_OutputFormatSpecs* output,
	const OP_Inputs* inputs,
	TOP_Context* context,
	void* reserved1)
{

	// if (myExecuteCount % 100 == 0) {

	// Lock the settings to make sure only this thread can access it
	mySettingsLock.lock();
	const double startDistance = inputs->getParDouble("Near");
	myStartDistance = startDistance;
	const double endDistance = inputs->getParDouble("Far");
	myEndDistance = endDistance;
	// Unlock them again
	mySettingsLock.unlock();

	// Sync the output
	myFrameQueue.sync(output);

	// Start a thread
	if (!myThread)
	{
		myThread = new std::thread(
			[this]()
			{
				// Exit when our owner tells us to
				while (!this->myThreadShouldExit)
				{
					int width, height;
					void* buf = this->myFrameQueue.getBufferForUpdate(&width, &height);

					// If there is a buffer to update
					if (buf)
					{

						pImage = pDevice->GetImage(imageTimeout);
						size_t bitsPerPixel = pImage->GetBitsPerPixel();
						Cpp_Acquisition::pImageToTop(pImage->GetData(), myStartDistance, myEndDistance, width, height, bitsPerPixel, coordinateScale, (float*)buf);

						this->myFrameQueue.updateComplete();
					}

				}
			}
		);
	}

		// }

	myFrameQueue.sendBufferForUpload(output);

}

void
Cpp_Acquisition::pImageToTop(const uint8_t* pInput, double startDistance, double endDistance, size_t width, size_t height, size_t srcBpp, float scale, float* pOut)
{
	size_t size = width * height;
	size_t srcPixelSize = srcBpp / 8; // divide by the number of bits in a byte

	const uint8_t* pIn = pInput;

	const double RGBmin = 0.0;
	const double RGBmax = 1.0;
	const double distance = endDistance - startDistance;

	const double redColorBorder = startDistance;
	const double yellowColorBorder = startDistance + (distance / 4);
	const double greenColorBorder = startDistance + ((distance / 4) * 2);
	const double cyanColorBorder = startDistance + ((distance / 4) * 3);
	const double blueColorBorder = endDistance;

	// iterate through each pixel and assign a color to it according to a distance
	for (size_t i = 0; i < size; i++)
	{
		// Isolate the z data
		//    The first channel is the x coordinate, second channel is the y coordinate,
		//    the third channel is the z coordinate (which is what we will use to determine
		//    the coloring) and the fourth channel is intensity.
		int16_t z = *reinterpret_cast<const int16_t*>((pIn + 4));

		// Convert z to millimeters
		//    The z data converts at a specified ratio to mm, so by multiplying it by the
		//    Scan3dCoordinateScale for CoordinateC, we are able to convert it to mm and
		//    can then compare it to the maximum distance of 6000mm.
		z = int16_t(double(z) * scale);

		double coordinateColorBlue = 0.0;
		double coordinateColorGreen = 0.0;
		double coordinateColorRed = 0.0;

		if (z >= startDistance && z <= endDistance) {

			// colors between red and yellow
			if ((z >= redColorBorder) && (z <= yellowColorBorder))
			{
				double yellowColorPercentage = (z - redColorBorder) / yellowColorBorder;

				coordinateColorBlue = RGBmin;
				coordinateColorGreen = RGBmax * yellowColorPercentage;
				coordinateColorRed = RGBmax;

			}

			// colors between yellow and green
			else if ((z > yellowColorBorder) && (z <= greenColorBorder))
			{
				double greenColorPercentage = (z - yellowColorBorder) / yellowColorBorder;

				coordinateColorBlue = RGBmin;
				coordinateColorGreen = RGBmax;
				coordinateColorRed = RGBmax - RGBmax * greenColorPercentage;
			}

			// colors between green and cyan
			else if ((z > greenColorBorder) && (z <= cyanColorBorder))
			{
				double cyanColorPercentage = (z - greenColorBorder) / yellowColorBorder;

				coordinateColorBlue = RGBmax * cyanColorPercentage;
				coordinateColorGreen = RGBmax;
				coordinateColorRed = RGBmin;

			}

			// colors between cyan and blue
			else if ((z > cyanColorBorder) && (z <= blueColorBorder))
			{
				double blueColorPercentage = (z - cyanColorBorder) / yellowColorBorder;

				coordinateColorBlue = RGBmax;
				coordinateColorGreen = RGBmax - RGBmax * blueColorPercentage;
				coordinateColorRed = RGBmin;
			}
			else
			{
				coordinateColorBlue = RGBmin;
				coordinateColorGreen = RGBmin;
				coordinateColorRed = RGBmin;
			}

		}

		int row = (height - (i / width) - 1);
		int column = i % width;

		float grayscale = (6000 -z ) / 6000.0;
		if (grayscale < 0.0) {
			grayscale = 0.0;
		}
		else if (grayscale > 1.0) {
			grayscale = 0.0;
		}

		// std::cout << i << ": " << row << " x " << column << "\n";
		float* pixel = &pOut[4 * (row * width + column)];
		pixel[0] = coordinateColorRed;
		pixel[1] = coordinateColorGreen;
		pixel[2] = coordinateColorBlue;
		pixel[3] = 1;
		
		pIn += srcPixelSize;

	}

	pDevice->RequeueBuffer(pImage);

}

void
Cpp_Acquisition::startMoreWork()
{
	{
		std::unique_lock<std::mutex> lck(myConditionLock);
		myStartWork = true;
	}
	myCondition.notify_one();
}

void
Cpp_Acquisition::waitForMoreWork()
{
	std::unique_lock<std::mutex> lck(myConditionLock);
	myCondition.wait(lck, [this]() { return this->myStartWork.load(); });
	myStartWork = false;
}

int32_t
Cpp_Acquisition::getNumInfoCHOPChans(void* reserved1)
{
	// We return the number of channel we want to output to any Info CHOP
	// connected to the TOP. In this example we are just going to send one channel.
	return 2;
}

void
Cpp_Acquisition::getInfoCHOPChan(int32_t index, OP_InfoCHOPChan* chan, void* reserved1)
{
	// This function will be called once for each channel we said we'd want to return
	// In this example it'll only be called once.

	if (index == 0)
	{
		chan->name->setString("executeCount");
		chan->value = (float)myExecuteCount;
	}

	if (index == 1)
	{
		chan->name->setString("step");
		chan->value = (float)myStep;
	}
}

bool
Cpp_Acquisition::getInfoDATSize(OP_InfoDATSize* infoSize, void* reserved1)
{
	infoSize->rows = 2;
	infoSize->cols = 2;
	// Setting this to false means we'll be assigning values to the table
	// one row at a time. True means we'll do it one column at a time.
	infoSize->byColumn = false;
	return true;
}

void
Cpp_Acquisition::getInfoDATEntries(int32_t index,
	int32_t nEntries,
	OP_InfoDATEntries* entries,
	void* reserved1)
{
	char tempBuffer[4096];

	if (index == 0)
	{
		// Set the value for the first column
#ifdef _WIN32
		strcpy_s(tempBuffer, "executeCount");
#else // macOS
		strlcpy(tempBuffer, "executeCount", sizeof(tempBuffer));
#endif
		entries->values[0]->setString(tempBuffer);

		// Set the value for the second column
#ifdef _WIN32
		sprintf_s(tempBuffer, "%d", myExecuteCount);
#else // macOS
		snprintf(tempBuffer, sizeof(tempBuffer), "%d", myExecuteCount);
#endif
		entries->values[1]->setString(tempBuffer);
	}

	if (index == 1)
	{
#ifdef _WIN32
		strcpy_s(tempBuffer, "step");
#else // macOS
		strlcpy(tempBuffer, "step", sizeof(tempBuffer));
#endif
		entries->values[0]->setString(tempBuffer);

#ifdef _WIN32
		sprintf_s(tempBuffer, "%g", myStep);
#else // macOS
		snprintf(tempBuffer, sizeof(tempBuffer), "%g", myStep);
#endif
		entries->values[1]->setString(tempBuffer);
	}
}

void
Cpp_Acquisition::setupParameters(OP_ParameterManager* manager, void* reserved1)
{
	// Near distance
	{
		OP_NumericParameter	np;

		np.name = "Near";
		np.label = "Near";
		np.defaultValues[0] = 0.0;

		np.minSliders[0] = 0.0;
		np.maxSliders[0] = 6000.0;

		np.minValues[0] = 0.0;
		np.maxValues[0] = 6000.0;

		np.clampMins[0] = true;
		np.clampMaxes[0] = true;

		OP_ParAppendResult res = manager->appendFloat(np);
		assert(res == OP_ParAppendResult::Success);
	}
	
	// Far distance
	{
		OP_NumericParameter	np;

		np.name = "Far";
		np.label = "Far";
		np.defaultValues[0] = 6000.0;

		np.minSliders[0] = 0.0;
		np.maxSliders[0] = 6000.0;

		np.minValues[0] = 0.0;
		np.maxValues[0] = 6000.0;

		np.clampMins[0] = true;
		np.clampMaxes[0] = true;

		OP_ParAppendResult res = manager->appendFloat(np);
		assert(res == OP_ParAppendResult::Success);
	}

	// pulse
	{
		OP_NumericParameter	np;

		np.name = "Reset";
		np.label = "Reset";

		OP_ParAppendResult res = manager->appendPulse(np);
		assert(res == OP_ParAppendResult::Success);
	}

}

void
Cpp_Acquisition::pulsePressed(const char* name, void* reserved1)
{
	if (!strcmp(name, "Reset"))
	{
	}


}

