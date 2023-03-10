#pragma once

struct FController
{
	void MouseMove(const WPARAM buttonState, const POINT position)
	{
		m_mouseButtonState = buttonState;
		m_mouseCurrentPosition = position;
	}

	bool KeyPress(int key) const
	{
		return (GetAsyncKeyState(key) & 0x8000) != 0;
	}

	bool MouseLeftButtonPressed() const
	{
		return m_mouseButtonState == MK_LBUTTON;
	}

	bool MouseRightButtonPressed() const
	{
		return m_mouseButtonState == MK_RBUTTON;
	}

	bool MoveForward() const
	{
		return MouseLeftButtonPressed() && KeyPress('W');
	}

	bool MoveBack() const
	{
		return MouseLeftButtonPressed() && KeyPress('S');
	}

	bool StrafeLeft() const
	{
		return MouseLeftButtonPressed() && KeyPress('A');
	}

	bool StrafeRight() const
	{
		return MouseLeftButtonPressed() && KeyPress('D');
	}

	float Pitch() const
	{
		return MouseLeftButtonPressed() ? DirectX::XMConvertToRadians((float)m_mouseMovement.y) : 0.f;
	}

	float Yaw() const
	{
		return MouseLeftButtonPressed() ? DirectX::XMConvertToRadians((float)m_mouseMovement.x) : 0.f;
	}

	float RotateSceneX() const
	{
		return MouseRightButtonPressed() ? DirectX::XMConvertToRadians((float)m_mouseMovement.x) : 0.f;
	}

	float RotateSceneY() const
	{
		return MouseRightButtonPressed() ? DirectX::XMConvertToRadians((float)m_mouseMovement.y) : 0.f;
	}

	void Tick(const float deltaTime)
	{
		m_mouseMovement = { m_mouseCurrentPosition.x - m_mouseLastPosition.x, m_mouseCurrentPosition.y - m_mouseLastPosition.y };
		m_mouseLastPosition = m_mouseCurrentPosition;
	}

	WPARAM m_mouseButtonState = {};
	POINT m_mouseCurrentPosition = { 0,0 };
	POINT m_mouseLastPosition = { 0,0 };
	POINT m_mouseMovement = { 0,0 };
};