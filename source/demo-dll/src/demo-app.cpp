#include <demo.h>
#include <profiling.h>
#include <backend-d3d12.h>
#include <shadercompiler.h>
#include <ui.h>
#include <ppltasks.h>
#include <ppl.h>

bool Demo::App::Initialize(const HWND& windowHandle, const uint32_t resX, const uint32_t resY)
{
	m_aspectRatio = resX / (float)resY;

	bool ok = RenderBackend12::Initialize(windowHandle, resX, resY, m_config);
	ok = ok && ShaderCompiler::Initialize();

	Renderer::Initialize(resX, resY);
	UI::Initialize(windowHandle);

	// List of models
	for (auto& entry : std::filesystem::recursive_directory_iterator(CONTENT_DIR))
	{
		if (entry.is_regular_file() &&
			entry.path().extension().string() == ".gltf" &&
			!entry.path().parent_path().string().ends_with(".model-cache"))
		{
			m_modelList.push_back(entry.path().filename().wstring());
		}
	}

	// List of HDRIs
	for (auto& entry : std::filesystem::recursive_directory_iterator(CONTENT_DIR))
	{
		if (entry.is_regular_file() && entry.path().extension().string() == ".hdr")
		{
			m_hdriList.push_back(entry.path().filename().wstring());
		}
	}

	return ok;
}

void Demo::App::Teardown(HWND& windowHandle)
{
	Renderer::Status::Pause();

	m_scene.Clear();

	Renderer::Teardown();
	RenderBackend12::FlushGPU();

	if (windowHandle)
	{
		RenderBackend12::Teardown();
		ShaderCompiler::Teardown();
		UI::Teardown();
	}
}

void Demo::App::Tick(const float deltaTime)
{
	SCOPED_CPU_EVENT("tick_demo", PIX_COLOR_INDEX(1));

	// Scene rotation
	static float rotX = 0.f;
	static float rotY = 0.f;

	// Reload scene model if required
	if (m_scene.m_modelFilename.empty() ||
		m_scene.m_modelFilename != m_config.ModelFilename)
	{
		// Async loading of model. Create a temp scene on the stack 
		// and replace the main scene once loading has finished.
		// --> A shared pointer is used to keep the temp scene alive and pass to the continuation task. 
		// --> The modelFilename is updated immediately to prevent subsequent reloads before the async reloading has finished.
		// TODO: support cancellation if a different model load is triggered
		std::shared_ptr<FScene> newScene = std::make_shared<FScene>();
		m_scene.m_modelFilename = m_config.ModelFilename;
		concurrency::task<void> loadSceneTask = concurrency::create_task([this, newScene]()
		{
			newScene->ReloadModel(m_config.ModelFilename);

			if (m_config.EnvSkyMode == (int)EnvSkyMode::HDRI)
			{
				newScene->ReloadEnvironment(m_config.HDRIFilename);
			}
		}).then([this, newScene]()
		{
			Renderer::Status::Pause();
			m_scene = std::move(*newScene);
			m_view.Reset(&m_scene);
			rotX = 0.f;
			rotY = 0.f;
			FScene::s_loadProgress = 1.f;
			Renderer::Status::Resume();
		});
	}

	// Reload scene environment if required
	if (m_config.EnvSkyMode == (int)EnvSkyMode::HDRI &&
		(m_scene.m_hdriFilename.empty() || m_scene.m_hdriFilename != m_config.HDRIFilename))
	{
		Renderer::Status::Pause();
		m_scene.ReloadEnvironment(m_config.HDRIFilename);
		FScene::s_loadProgress = 1.f;
		Renderer::Status::Resume();
	}

	// Tick components
	m_controller.Tick(deltaTime);
	m_view.Tick(deltaTime, &m_controller);
	if (!m_config.FreezeCulling)
	{
		m_cullingView = m_view;
	}

	// Handle scene rotation
	{
		// Mouse rotation but as applied in view space
		Matrix rotation = Matrix::Identity;

		float newRotX = m_controller.RotateSceneX();
		float newRotY = m_controller.RotateSceneY();
		if (newRotX != 0.f || newRotY != 0.f)
		{
			Renderer::ResetPathtraceAccumulation();
		}

		rotX -= newRotX;
		if (rotX != 0.f)
		{
			rotation *= Matrix::CreateFromAxisAngle(m_view.m_up, rotX);
		}

		rotY -= newRotY;
		if (rotY != 0.f)
		{
			rotation *= Matrix::CreateFromAxisAngle(m_view.m_right, rotY);
		}

		// Rotate to view space, apply view space rotation and then rotate back to world space
		m_scene.m_rootTransform = rotation;
	}

	UI::Update(this, deltaTime);
}

void Demo::App::OnMouseMove(WPARAM buttonState, int x, int y)
{
	m_controller.MouseMove(buttonState, POINT{ x, y });
}

void Demo::App::Render(const uint32_t resX, const uint32_t resY) const
{
	// Create a immutable copy of the demo state for the renderer to use
	auto CreateRenderState = [=]()
	{
		FRenderState s;
		s.m_config = m_config;
		s.m_scene = &m_scene;
		s.m_view = m_view;
		s.m_cullingView = m_cullingView;
		s.m_resX = resX;
		s.m_resY = resY;
		s.m_mouseX = m_controller.m_mouseCurrentPosition.x;
		s.m_mouseY = m_controller.m_mouseCurrentPosition.y;
		return s;
	};

	if (!Renderer::Status::IsPaused())
	{
		const FRenderState renderState = CreateRenderState();
		Renderer::Render(renderState);
	}
}

const FConfig& Demo::App::GetConfig() const
{
	return m_config;
}