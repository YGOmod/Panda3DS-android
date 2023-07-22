#include "renderer.hpp"
#include "vulkan_api.hpp"

class GPU;

class RendererVK final : public Renderer {
	vk::UniqueInstance instance = {};
	vk::PhysicalDevice physicalDevice = {};
	vk::UniqueDevice device = {};

	vk::UniqueSurfaceKHR surface = {};

	vk::UniqueSwapchainKHR swapchain = {};
	std::vector<vk::UniqueImage> swapchainImages = {};
	std::vector<vk::UniqueImageView> swapchainImageViews = {};

	vk::UniqueDebugUtilsMessengerEXT debugMessenger = {};

	vk::Queue presentQueue = {};
	u32 presentQueueFamily = ~0u;
	vk::Queue graphicsQueue = {};
	u32 graphicsQueueFamily = ~0u;
	vk::Queue computeQueue = {};
	u32 computeQueueFamily = ~0u;
	vk::Queue transferQueue = {};
	u32 transferQueueFamily = ~0u;

  public:
	RendererVK(GPU& gpu, const std::array<u32, regNum>& internalRegs);
	~RendererVK() override;

	void reset() override;
	void display() override;
	void initGraphicsContext(SDL_Window* window) override;
	void clearBuffer(u32 startAddress, u32 endAddress, u32 value, u32 control) override;
	void displayTransfer(u32 inputAddr, u32 outputAddr, u32 inputSize, u32 outputSize, u32 flags) override;
	void drawVertices(PICA::PrimType primType, std::span<const PICA::Vertex> vertices) override;
	void screenshot(const std::string& name) override;
};