/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2025 - Mario Bergeron <Mario.Bergeron@avnet.com>
 *
 * Pipeline handler for AMD/Xilinx HSL-based ISP implemented on Zynq-UltraScale+
 *   and/or
 * Pipeline handler for AMD/Xilinx AIE-ML-based ISP implemented on Versal AI Edge 
 */

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <libcamera/base/log.h>
#include <libcamera/base/utils.h>

#include <libcamera/camera_manager.h>
#include <libcamera/formats.h>
#include <libcamera/geometry.h>
#include <libcamera/stream.h>

#include "libcamera/internal/bayer_format.h"
#include "libcamera/internal/camera.h"
#include "libcamera/internal/camera_sensor.h"
#include "libcamera/internal/device_enumerator.h"
#include "libcamera/internal/media_device.h"
#include "libcamera/internal/pipeline_handler.h"
#include "libcamera/internal/v4l2_subdevice.h"
#include "libcamera/internal/v4l2_videodevice.h"

#include "linux/media-bus-format.h"

namespace libcamera {

LOG_DEFINE_CATEGORY(XISP)

class PipelineHandlerXISP;

class XISPCameraData : public Camera::Private
{
public:
	XISPCameraData(PipelineHandler *ph)
		: Camera::Private(ph)
	{
		/*
		 * \todo Assume 1 channel only for now, as that's the number of
		 * available channels our current implementation.
		 */
		streams_.resize(1);
	}

	PipelineHandlerXISP *pipe();

	int init();

	unsigned int pipeIndex(const Stream *stream)
	{
		return stream - &*streams_.begin();
	}

	std::unique_ptr<CameraSensor> camSensor_;

	std::vector<Stream> streams_;

	std::vector<Stream *> enabledStreams_;
};

class XISPCameraConfiguration : public CameraConfiguration
{
public:
	XISPCameraConfiguration(XISPCameraData *data)
		: data_(data)
	{
	}

	Status validate() override;

	static const std::map<PixelFormat, unsigned int> formatsMap_;

	V4L2SubdeviceFormat sensorFormat_;

private:
	const XISPCameraData *data_;
};

class PipelineHandlerXISP : public PipelineHandler
{
public:
	PipelineHandlerXISP(CameraManager *manager);

	bool match(DeviceEnumerator *enumerator) override;

	std::unique_ptr<CameraConfiguration>
	generateConfiguration(Camera *camera, Span<const StreamRole> roles) override;
	int configure(Camera *camera, CameraConfiguration *config) override;

	int exportFrameBuffers(Camera *camera, Stream *stream,
			       std::vector<std::unique_ptr<FrameBuffer>> *buffers) override;

	int start(Camera *camera, const ControlList *controls) override;

protected:
	void stopDevice(Camera *camera) override;

	int queueRequestDevice(Camera *camera, Request *request) override;

private:
	static constexpr Size kPreviewSize = { 1920, 1080 };
	static constexpr Size kMinXISPSize = { 64, 64 };
	static constexpr Size kMaxXISPSize = { 4096, 4096 };

	struct Pipe {
    std::unique_ptr<V4L2Subdevice> resizer;
		std::unique_ptr<V4L2VideoDevice> capture;
	};

	XISPCameraData *cameraData(Camera *camera)
	{
		return static_cast<XISPCameraData *>(camera->_d());
	}

	Pipe *pipeFromStream(Camera *camera, const Stream *stream);

	StreamConfiguration generateYUVConfiguration(Camera *camera,
						     const Size &size);
	StreamConfiguration generateRawConfiguration(Camera *camera);

	void bufferReady(FrameBuffer *buffer);

	MediaDevice *mediaDev_;

  Size sensorBestSize_;
  unsigned int sensorBestFormatCode_;
  //ColorSpace sensorBestColorSpace_;
	std::unique_ptr<V4L2Subdevice> sensor_;
	std::unique_ptr<V4L2Subdevice> vcm_;
	std::unique_ptr<V4L2Subdevice> csi2rx_;
	std::unique_ptr<V4L2Subdevice> xisp_;

	std::vector<Pipe> pipes_;
};

/* -----------------------------------------------------------------------------
 * Camera Data
 */

PipelineHandlerXISP *XISPCameraData::pipe()
{
	return static_cast<PipelineHandlerXISP *>(Camera::Private::pipe());
}

/* Open and initialize pipe components. */
int XISPCameraData::init()
{
#if 0 // 0.3.2 implementation
	int ret = camSensor_->init();
	if (ret)
		return ret;
#else // 0.4.0 implementation
	if (!camSensor_)
		return -ENODEV;
#endif

	properties_ = camSensor_->properties();

	return 0;
}


/* -----------------------------------------------------------------------------
 * Camera Configuration
 */

/*
 * XISPCameraConfiguration::formatsMap_ records the association between an output
 * pixel format and the xisp source pixel format to be applied to the pipeline.
 */
const std::map<PixelFormat, unsigned int> XISPCameraConfiguration::formatsMap_ = {
	{ formats::YUYV, MEDIA_BUS_FMT_YUV8_1X24 },
	{ formats::RGB888, MEDIA_BUS_FMT_RGB888_1X24 },
	{ formats::BGR888, MEDIA_BUS_FMT_BGR888_1X24 },
	{ formats::RBG888, MEDIA_BUS_FMT_RBG888_1X24 },
};

CameraConfiguration::Status XISPCameraConfiguration::validate()
{
	LOG(XISP, Debug) << "[PipelineHandlerXISP::validate] Validating Configuration";  

	Status status = Valid;

	std::set<Stream *> availableStreams;
	std::transform(data_->streams_.begin(), data_->streams_.end(),
		       std::inserter(availableStreams, availableStreams.end()),
		       [](const Stream &s) { return const_cast<Stream *>(&s); });

	if (config_.empty())
		return Invalid;

  LOG(XISP, Debug) << "  [data_->streams_.size()] " << data_->streams_.size();
  LOG(XISP, Debug) << "  [availableStreams.size()] " << availableStreams.size();
  LOG(XISP, Debug) << "  [config_.size()] " << config_.size();

	/* Cap the number of streams to the number of available xisp pipes. */
	if (config_.size() > availableStreams.size()) {
		config_.resize(availableStreams.size());
		status = Adjusted;
	}

	/* Cap the number of streams to the number of available xisp pipes. */
	if (config_.size() > 1) {
		config_.resize(1);
		status = Adjusted;
	}

  LOG(XISP, Debug) << "  [config_.size()] " << config_.size();

	//CameraSensor *sensor = data_->camSensor_.get();
	//Size maxResolution = sensor->resolution();

	/* Validate streams according to the format of the first one. */
	const PixelFormatInfo info = PixelFormatInfo::info(config_[0].pixelFormat);

	for (const auto &[i, config] : utils::enumerate(config_)) {
		/* Assign streams in the order they are presented. */
		auto stream = availableStreams.extract(availableStreams.begin());
		config.setStream(stream.value());

		config.stride = info.stride(config.size.width, 0);
		config.frameSize = info.frameSize(config.size, info.bitsPerPixel);
 
		LOG(XISP, Debug) << "  Stream " << i << ": " << config.toString();
		//LOG(XISP, Debug) << "    [config] : " << config;
		LOG(XISP, Debug) << "    [config.size] : " << config.size;
		LOG(XISP, Debug) << "    [config.pixelFormat] : " << config.pixelFormat;
		LOG(XISP, Debug) << "    [config.stride] : " << config.stride;
		LOG(XISP, Debug) << "    [config.frameSize] : " << config.frameSize;
  }

	/*
	 * Sensor format selection policy: the first stream selects the media
	 * bus code to use, the largest stream selects the size.
	 *
	 * \todo The sensor format selection policy could be changed to
	 * prefer operating the sensor at full resolution to prioritize
	 * image quality in exchange of a usually slower frame rate.
	 * Usage of the STILL_CAPTURE role could be consider for this.
	 */
	Size maxSize;
	for (const auto &cfg : config_) {
		if (cfg.size > maxSize)
			maxSize = cfg.size;
	}

	//PixelFormat pixelFormat = config_[0].pixelFormat;

	V4L2SubdeviceFormat sensorFormat{};
	//sensorFormat.code = data_->getMediaBusFormat(&pixelFormat);
	//	MEDIA_BUS_FMT_UYVY8_1X16,
	//	MEDIA_BUS_FMT_YUV8_1X24,
	//	MEDIA_BUS_FMT_RGB565_1X16,
	//	MEDIA_BUS_FMT_RGB888_1X24,
  //sensorFormat.code = MEDIA_BUS_FMT_UYVY8_1X16;
  sensorFormat.code = MEDIA_BUS_FMT_RBG888_1X24;
  //sensorFormat.code = MEDIA_BUS_FMT_RGB888_1X24;
	sensorFormat.size = maxSize;

	LOG(XISP, Debug) << "Computed sensor configuration: " << sensorFormat;

	sensorFormat_.code = sensorFormat.code;
	sensorFormat_.size = sensorFormat.size;

	LOG(XISP, Debug) << "Selected sensor format: " << sensorFormat_;

	return status;
}

/* -----------------------------------------------------------------------------
 * Pipeline Handler
 */

PipelineHandlerXISP::PipelineHandlerXISP(CameraManager *manager)
	: PipelineHandler(manager)
{
}


std::unique_ptr<CameraConfiguration>
PipelineHandlerXISP::generateConfiguration(Camera *camera,
					  Span<const StreamRole> roles)
{
	XISPCameraData *data = cameraData(camera);
	std::unique_ptr<XISPCameraConfiguration> config =
		std::make_unique<XISPCameraConfiguration>(data);

  LOG(XISP, Debug) << "[PipelineHandlerXISP::generateConfiguration] Generate Configuration";  

	if (roles.empty())
		return config;

  LOG(XISP, Debug) << "  [roles.size()] " << roles.size();
  LOG(XISP, Debug) << "  [data->streams_.size()] " << data->streams_.size();
  
	if (roles.size() > data->streams_.size()) {
		LOG(XISP, Error) << "Only up to " << data->streams_.size()
				<< " streams are supported";
		return nullptr;
	}

	for (const auto &role : roles) {
    switch (role) {
      case StreamRole::StillCapture: {
        LOG(XISP, Debug) << "  [role] StilCapture";
        break;
      }      
      case StreamRole::Viewfinder: {
        LOG(XISP, Debug) << "  [role] Viewfinder";
        break;
      }      
      case StreamRole::VideoRecording: {
        LOG(XISP, Debug) << "  [role] VideoRecording";
        break;
      }      
      case StreamRole::Raw: {
        LOG(XISP, Debug) << "  [role] Raw";
        break;
      }      
		  default: {
        LOG(XISP, Error) << "Requested stream role not supported: " << role;
        return nullptr;
      }
		}
  }

	/*
	 * Populate the StreamConfiguration.
	 *
	 * As the sensor supports at least one YUV/RGB media bus format all the
	 * processed ones in formatsMap_ can be generated from it.
	 */
	std::map<PixelFormat, std::vector<SizeRange>> streamFormats;
	for (const auto &[pixFmt, pipeFmt] : XISPCameraConfiguration::formatsMap_) {
		//const PixelFormatInfo &info = PixelFormatInfo::info(pixFmt);
		streamFormats[pixFmt] = { { kMinXISPSize, kMaxXISPSize } };
	}
	for (auto const &streamFormat : streamFormats) {
    LOG(XISP, Debug) << "  [streamFormat] " << streamFormat.first;
    for (auto const &sizeRange : streamFormat.second ) {
      LOG(XISP, Debug) << "    [sizeRange] " << sizeRange;    
    } 
  }
 
	StreamFormats formats(streamFormats);
	for (auto const &pixelFormat : formats.pixelformats()) {
    LOG(XISP, Debug) << "  [formats.pixelformats()] " << pixelFormat;
  	for (auto const &size : formats.sizes(pixelFormat)) {
      LOG(XISP, Debug) << "    [formats.sizes(" << pixelFormat << ")] " << size;
    }        
  }
 
  StreamConfiguration cfg(formats);
  //LOG(XISP, Debug) << "  [cfg] : " << cfg.toString();

  //cfg.size = {1920,1080};
  //cfg.size = {1280,720};
  cfg.size = {640,480};
  //cfg.pixelFormat = formats::YUYV;
  //cfg.pixelFormat = formats::BGR888;
  //cfg.pixelFormat = formats::RGB888;
  cfg.pixelFormat = formats::RBG888;
  
	const PixelFormatInfo info = PixelFormatInfo::info(cfg.pixelFormat);
	cfg.stride = info.stride(cfg.size.width, 0);
	cfg.frameSize = info.frameSize(cfg.size, info.bitsPerPixel);

  cfg.bufferCount = 4;

  LOG(XISP, Debug) << "  [cfg] : " << cfg.toString();
  LOG(XISP, Debug) << "    [cfg.size] : " << cfg.size;
  LOG(XISP, Debug) << "    [cfg.pixelFormat] : " << cfg.pixelFormat;
  LOG(XISP, Debug) << "    [cfg.stride] : " << cfg.stride;
  LOG(XISP, Debug) << "    [cfg.frameSize] : " << cfg.frameSize;
        
	config->addConfiguration(cfg);
	config->validate();

	return config;
}

int PipelineHandlerXISP::configure(Camera *camera, CameraConfiguration *c)
{
	XISPCameraConfiguration *camConfig = static_cast<XISPCameraConfiguration *>(c);
	XISPCameraData *data = cameraData(camera);

  LOG(XISP, Debug) << "[PipelineHandlerXISP::configure] Configure Camera";  

	/* All links are immutable except the sensor -> csis link. */
	const MediaPad *sensorSrc = data->camSensor_->entity()->getPadByIndex(0);
	sensorSrc->links()[0]->setEnabled(true);

  int ret;
  
  // Defined a fixed format
  V4L2SubdeviceFormat csi2rxFormat{};
  V4L2SubdeviceFormat xispFormat{};
	V4L2SubdeviceFormat vpssFormat{};
	V4L2DeviceFormat    captureFormat{};
  
  csi2rxFormat.code = sensorBestFormatCode_;
  csi2rxFormat.size.width  = sensorBestSize_.width;
  csi2rxFormat.size.height = sensorBestSize_.height;
  //csi2rxFormat.colorSpace = ColorSpace::Srgb;

  xispFormat.code = MEDIA_BUS_FMT_RBG888_1X24;
  //xispFormat.code = MEDIA_BUS_FMT_RGB888_1X24;
  xispFormat.size.width  = sensorBestSize_.width;
  xispFormat.size.height = sensorBestSize_.height;
  //xispFormat.colorSpace = ColorSpace::Srgb;
  
	vpssFormat = camConfig->sensorFormat_;
  vpssFormat.code = MEDIA_BUS_FMT_RBG888_1X24;
  //vpssFormat.code = MEDIA_BUS_FMT_RGB888_1X24;
  //vpssFormat.colorSpace = ColorSpace::Srgb;
   
	/* Apply format to the sensor and CSIS receiver. */
	ret = data->camSensor_->setFormat(&csi2rxFormat);
	if (ret)
		return ret;

  LOG(XISP, Debug) << "  [CSI ] : " << csi2rxFormat;
	//ret = data->csi2rx_->setFormat(0, &format);
  ret = csi2rx_->setFormat(0, &csi2rxFormat);
	if (ret)
		return ret;
  ret = csi2rx_->setFormat(1, &csi2rxFormat);
	if (ret)
		return ret;

  LOG(XISP, Debug) << "  [XISP] : " << xispFormat;
  ret = xisp_->setFormat(0, &csi2rxFormat);
	if (ret)
		return ret;
  ret = xisp_->setFormat(1, &xispFormat);
	if (ret)
		return ret;

	/* Now configure the resizer and video node instances, one per stream. */
	data->enabledStreams_.clear();
 
	//for (const auto &config : *c) {
 	for (const auto &[i, config] : utils::enumerate(*c)) {
		LOG(XISP, Debug) << "  Stream " << i << ": " << config.toString();
    //LOG(XISP, Debug) << "    [config] : " << config;
    LOG(XISP, Debug) << "    [config.size] : " << config.size;
    LOG(XISP, Debug) << "    [config.pixelFormat] : " << config.pixelFormat;
    LOG(XISP, Debug) << "    [config.stride] : " << config.stride;
    LOG(XISP, Debug) << "    [config.frameSize] : " << config.frameSize;
		
    //Pipe *pipe = pipeFromStream(camera, config.stream());
    Pipe *pipe = &pipes_[0];

    LOG(XISP, Debug) << "  [VPSS] : " << vpssFormat;
		//ret = pipe->xisp->setFormat(0, &format);
    ret = pipe->resizer->setFormat(0, &xispFormat);
		if (ret)
			return ret;
    ret = pipe->resizer->setFormat(1, &vpssFormat);
		if (ret)
			return ret;

  	const PixelFormatInfo &info = PixelFormatInfo::info(config.pixelFormat);
  	captureFormat.fourcc = pipe->capture->toV4L2PixelFormat(config.pixelFormat);
	  captureFormat.size = config.size;
    captureFormat.planesCount = info.numPlanes();
    captureFormat.planes[0].bpl = config.stride;

    LOG(XISP, Debug) << "  [VCAP] : " << captureFormat;
    LOG(XISP, Debug) << "    [captureFormat] : " << captureFormat.toString();
    LOG(XISP, Debug) << "      [captureFormat.planesCount] : " << captureFormat.planesCount;
    //LOG(XISP, Debug) << "      [captureFormat.planes[0].bpl] : " << captureFormat.planes[0].bpl;
		/* \todo Set stride and format. */
		ret = pipe->capture->setFormat(&captureFormat);
		if (ret)
			return ret;
      
    //if (captureFormat.size != config.size)
    //  return -EINVAL;

		/* Store the list of enabled streams for later use. */
		data->enabledStreams_.push_back(config.stream());
	}

	return 0;
}

int PipelineHandlerXISP::exportFrameBuffers(Camera *camera, Stream *stream,
					   std::vector<std::unique_ptr<FrameBuffer>> *buffers)
{
	unsigned int count = stream->configuration().bufferCount;
	Pipe *pipe = pipeFromStream(camera, stream);

	return pipe->capture->exportBuffers(count, buffers);
}

int PipelineHandlerXISP::start(Camera *camera,
			      [[maybe_unused]] const ControlList *controls)
{
	XISPCameraData *data = cameraData(camera);

	for (const auto &stream : data->enabledStreams_) {
		Pipe *pipe = pipeFromStream(camera, stream);
		const StreamConfiguration &config = stream->configuration();

		int ret = pipe->capture->importBuffers(config.bufferCount);
		if (ret)
			return ret;

		ret = pipe->capture->streamOn();
		if (ret)
			return ret;
	}

	return 0;
}

void PipelineHandlerXISP::stopDevice(Camera *camera)
{
	XISPCameraData *data = cameraData(camera);

	for (const auto &stream : data->enabledStreams_) {
		Pipe *pipe = pipeFromStream(camera, stream);

		pipe->capture->streamOff();
		pipe->capture->releaseBuffers();
	}
}

int PipelineHandlerXISP::queueRequestDevice(Camera *camera, Request *request)
{
	for (auto &[stream, buffer] : request->buffers()) {
		Pipe *pipe = pipeFromStream(camera, stream);

		int ret = pipe->capture->queueBuffer(buffer);
		if (ret)
			return ret;
	}

	return 0;
}

bool PipelineHandlerXISP::match(DeviceEnumerator *enumerator)
{
  // Additional context to what is being searched
  //   driver =  "xilinx-video"
  //
  // Capture Pipeline 0
  //   V4l2VideoDevice = "vcap_mipi_0_v_proc output 0"
  //   V4l2Subdevice = "imx219 1-0010"
  //   V4l2Subdevice = "80050000.mipi_csi2_rx_subsystem"
  //   V4l2Subdevice = "a0010000.ISPPipeline_accel"
  //   V4l2Subdevice = "a0040000.v_proc_ss"
  //
  // Capture Pipeline 1
  //   V4l2VideoDevice = "vcap_mipi_1_v_proc output 0"
  //   V4l2Subdevice = "imx708"
  //   V4l2Subdevice = "dw9807 2-000c"
  //   V4l2Subdevice = "80051000.mipi_csi2_rx_subsystem"
  //   V4l2Subdevice = "a0030000.ISPPipeline_accel"
  //   V4l2Subdevice = "a00c0000.v_proc_ss"
  // 
  // Capture Pipeline 2
  //   V4l2VideoDevice = "vcap_mipi_2_v_proc output 0"
  //   V4l2Subdevice = "imx500 3-001a"
  //   V4l2Subdevice = "80052000.mipi_csi2_rx_subsystem"
  //   V4l2Subdevice = "a0090000.ISPPipeline_accel"
  //   V4l2Subdevice = "a0100000.v_proc_ss"
  //
  // Capture Pipeline 3
  //   V4l2VideoDevice = "vcap_mipi_3_v_proc output 0"
  //   V4l2Subdevice = "imx477 4-001a"
  //   V4l2Subdevice = "80053000.mipi_csi2_rx_subsystem"
  //   V4l2Subdevice = "a00b0000.ISPPipeline_accel"
  //   V4l2Subdevice = "a0180000.v_proc_ss"

  LOG(XISP, Debug) << "[PipelineHandlerXISP::match] Looking for capture pipeline";  
	for (unsigned int i = 0; i < 4; i++) {
		std::string entityName = "vcap_mipi_" + std::to_string(i) + "_v_proc output 0";

    DeviceMatch dm("xilinx-video"); // driver
	  dm.add(entityName); // entity

    mediaDev_ = acquireMediaDevice(enumerator, dm);
    if (mediaDev_) {
      LOG(XISP, Debug) << "  Found pipeline ... ";  
      break;
    }
	  if (!mediaDev_) {
		  continue;
    }
  }
  if (!mediaDev_) {
    LOG(XISP, Debug) << "  Done ...";  
    return false;
  }
    
  int ret;

  MediaEntity *sensor_entity = NULL;
	std::unique_ptr<V4L2Subdevice> resizer = NULL;
 	std::unique_ptr<V4L2VideoDevice> capture = NULL;
   
  // Scan for entities in capture pipeline 
  //for (const MediaEntity *entity : mediaDev_->entities()) {
  for ( MediaEntity *entity : mediaDev_->entities()) {
	  if ( entity->name().find("imx") != std::string::npos ) {
      sensor_entity = entity;      
      LOG(XISP, Debug) << "  [CAM ] : " << entity->name();  
      if ( entity->name().find("imx219") != std::string::npos ) {
        sensorBestSize_.width = 1920;     
        sensorBestSize_.height = 1080; 
        sensorBestFormatCode_ = MEDIA_BUS_FMT_SRGGB10_1X10;       
        LOG(XISP, Debug) << "    [IMX219] : " << sensorBestSize_.width << "x" << sensorBestSize_.height << "-SRGGB10_1X10";  
        //sensorBestColorSpace_ = ColorSpace::Srgb;
      }   
      if ( entity->name().find("imx708") != std::string::npos ) {
        sensorBestSize_.width = 1536;     
        sensorBestSize_.height = 864;         
        sensorBestFormatCode_ = MEDIA_BUS_FMT_SRGGB10_1X10;       
        LOG(XISP, Debug) << "    [IMX708] : " << sensorBestSize_.width << "x" << sensorBestSize_.height << "-SRGGB10_1X10";  
        //sensorBestColorSpace_ = ColorSpace::Srgb;
      }   
      if ( entity->name().find("imx477") != std::string::npos ) {
        sensorBestSize_.width = 1332;     
        sensorBestSize_.height = 990;         
        sensorBestFormatCode_ = MEDIA_BUS_FMT_SRGGB10_1X10;       
        LOG(XISP, Debug) << "    [IMX477] : " << sensorBestSize_.width << "x" << sensorBestSize_.height << "-SRGGB10_1X10";
        //sensorBestColorSpace_ = ColorSpace::Srgb;
      }   
      if ( entity->name().find("imx500") != std::string::npos ) {
        sensorBestSize_.width = 2028;     
        sensorBestSize_.height = 1520;         
        sensorBestFormatCode_ = MEDIA_BUS_FMT_SRGGB10_1X10;       
        LOG(XISP, Debug) << "    [IMX500] : " << sensorBestSize_.width << "x" << sensorBestSize_.height << "-SRGGB10_1X10";  
        //sensorBestColorSpace_ = ColorSpace::Srgb;
      }   
      sensor_ = V4L2Subdevice::fromEntityName(mediaDev_, entity->name());
    }
	  if ( entity->name().find("dw9807") != std::string::npos ) {
      LOG(XISP, Debug) << "  [VCM ] : " << entity->name();         
      vcm_ = V4L2Subdevice::fromEntityName(mediaDev_, entity->name());        
    }
	  if ( entity->name().find("mipi_csi2_rx_subsystem") != std::string::npos ) {
      LOG(XISP, Debug) << "  [CSI ] : " << entity->name();           
      csi2rx_ = V4L2Subdevice::fromEntityName(mediaDev_, entity->name());
      //csi2rx_entity = entity;        
    }
	  if ( entity->name().find("ISPPipeline_accel") != std::string::npos ) {
      LOG(XISP, Debug) << "  [XISP] : " << entity->name();
      xisp_ = V4L2Subdevice::fromEntityName(mediaDev_, entity->name());        
    }
	  if ( entity->name().find("v_proc_ss") != std::string::npos ) {
      LOG(XISP, Debug) << "  [VPSS] : " << entity->name();
      resizer = V4L2Subdevice::fromEntityName(mediaDev_, entity->name());  
    }
	  if ( entity->name().find("vcap_mipi_") != std::string::npos ) {
      LOG(XISP, Debug) << "  [VCAP] : " << entity->name();  
  		capture = V4L2VideoDevice::fromEntityName(mediaDev_, entity->name());
    }
  }

	if (!csi2rx_)
	  return false;
	ret = csi2rx_->open();
	if (ret)
		return false;
  
	if (!xisp_)
	  return false;
	ret = xisp_->open();
	if (ret)
		return false;

  if (!resizer)
    return false;

	ret = resizer->open();
	if (ret)
		return false;
        
	if (!capture)
		return false;

	capture->bufferReady.connect(this, &PipelineHandlerXISP::bufferReady);

	ret = capture->open();
	if (ret)
		return ret;

	pipes_.push_back({ std::move(resizer), std::move(capture) });

	if (pipes_.empty()) {
		LOG(XISP, Error) << "Unable to enumerate pipes";
		return false;
	}

	if (sensor_entity->function() != MEDIA_ENT_F_CAM_SENSOR) {
		LOG(XISP, Debug) << "Skip unsupported subdevice "
				<< sensor_entity->name();
		return false;
	}

	/* Create the camera data. */
	std::unique_ptr<XISPCameraData> data =
		std::make_unique<XISPCameraData>(this);

#if 0 // 0.3.2 implementation
	data->camSensor_ = std::make_unique<CameraSensor>(sensor_entity);
#else // 0.4.0 implementation
	data->camSensor_ = CameraSensorFactoryBase::create(sensor_entity);
#endif
	//data->csi2rx_ = std::make_unique<V4L2Subdevice>(csi2rx_entity);

	ret = data->init();
	if (ret) {
		LOG(XISP, Error) << "Failed to initialize camera data";
		return false;
	}

	/* Register the camera. */
  LOG(XISP, Debug) << "Register the camera ...";
	const std::string &id = data->camSensor_->id();
  LOG(XISP, Debug) << "  [id] : " << id;
	std::set<Stream *> streams;
	std::transform(data->streams_.begin(), data->streams_.end(),
		       std::inserter(streams, streams.end()),
		       [](Stream &s) { return &s; });
	LOG(XISP, Debug) << "  [streams.size()] : " << streams.size();

	std::shared_ptr<Camera> camera =
		Camera::create(std::move(data), id, streams);

	registerCamera(std::move(camera));

  return true;
}

PipelineHandlerXISP::Pipe *PipelineHandlerXISP::pipeFromStream(Camera *camera,
							     const Stream *stream)
{
  //LOG(XISP, Debug) << "[pipeFromStream]";
  
	XISPCameraData *data = cameraData(camera);
	unsigned int pipeIndex = data->pipeIndex(stream);
	//LOG(XISP, Debug) << "  [pipeIndex] : " << pipeIndex;

	//ASSERT(pipeIndex < pipes_.size());
  // always 0 ... correct ... 

	return &pipes_[pipeIndex];
}

void PipelineHandlerXISP::bufferReady(FrameBuffer *buffer)
{
	Request *request = buffer->request();

	/* Record the sensor's timestamp in the request metadata. */
	ControlList &metadata = request->metadata();
	if (!metadata.contains(controls::SensorTimestamp.id()))
		metadata.set(controls::SensorTimestamp,
			     buffer->metadata().timestamp);

	completeBuffer(request, buffer);
	if (request->hasPendingBuffers())
		return;

	completeRequest(request);
}

REGISTER_PIPELINE_HANDLER(PipelineHandlerXISP, "xisp")

} /* namespace libcamera */
