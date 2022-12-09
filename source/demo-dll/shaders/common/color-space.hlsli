// http://paulbourke.net/miscellaneous/colourspace/
float3 hsv2rgb(float3 hsv)
{
	float3 rgb, sat;

	while (hsv.x < 0)
	{
		hsv.x += 360;
	}

	while (hsv.x > 360)
	{
		hsv.x -= 360;
	}

	if (hsv.x < 120)
	{
		sat.r = (120 - hsv.x) / 60.0;
		sat.g = hsv.x / 60.0;
		sat.b = 0;
	}
	else if (hsv.x < 240)
	{
		sat.r = 0;
		sat.g = (240 - hsv.x) / 60.0;
		sat.b = (hsv.x - 120) / 60.0;
	}
	else
	{
		sat.r = (hsv.x - 240) / 60.0;
		sat.g = 0;
		sat.b = (360 - hsv.x) / 60.0;
	}

	sat.r = min(sat.r, 1.f);
	sat.g = min(sat.g, 1.f);
	sat.b = min(sat.b, 1.f);

	rgb.r = (1 - hsv.y + hsv.y * sat.r) * hsv.z;
	rgb.g = (1 - hsv.y + hsv.y * sat.g) * hsv.z;
	rgb.b = (1 - hsv.y + hsv.y * sat.b) * hsv.z;

	return(rgb);
}