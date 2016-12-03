
#include "Settings.h"

namespace vv
{
	Settings* Settings::instance_ = nullptr;

	///////////////////////////////////////////////////////////////////////////////////////////// Public
	void Settings::setDefault()
	{
		default_ = true;
		window_width_ = 1920;
		window_height_ = 1080;
		application_name_ = "VirtualVistaVulkan";
		engine_name_ = "VirtualVista";

		graphics_required_ = true;
		compute_required_ = false;
		on_screen_rendering_required_ = true;
	}


	int Settings::getWindowWidth()
	{
		return window_width_;
	}


	int Settings::getWindowHeight()
	{
		return window_height_;
	}


	std::string Settings::getApplicationName()
	{
		return application_name_;
	}


	std::string Settings::getEngineName()
	{
		return engine_name_;
	}


	bool Settings::isGraphicsRequired()
	{
		return graphics_required_;
	}


	bool Settings::isComputeRequired()
	{
		return compute_required_;
	}


	bool Settings::isOnScreenRenderingRequired()
	{
		return on_screen_rendering_required_;
	}


	void Settings::setWindowWidth(int width)
	{
		window_width_ = width;
	}


	void Settings::setWindowHeight(int height)
	{
		window_height_ = height;
	}


	///////////////////////////////////////////////////////////////////////////////////////////// Private
	Settings* Settings::inst()
	{
		if (!instance_)
		{
			instance_ = new Settings;
			instance_->setDefault();
		}
		return instance_;
	}
}