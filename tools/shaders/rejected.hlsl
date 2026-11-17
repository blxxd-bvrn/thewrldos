// Deliberately unsupported profiles/resources, for upload rejection tests.
cbuffer ForeignSpace : register(b0, space2) { float4 OtherTint; };
float4 resource_space(float4 value : COLOR0) : SV_Target { return value * OtherTint; }
RWTexture2D<float4> Writable : register(u0);
float4 uav(float4 value : COLOR0) : SV_Target { Writable[uint2(0, 0)] = value; return value; }
[noinline] float helper(float value) { return value * value + 1; }
float4 helper_call(float4 value : COLOR0) : SV_Target { return helper(value.x); }
float4 double_math(float4 value : COLOR0) : SV_Target { double v = (double)value.x + 0.123456789012345; return (float)(v * v); }
float4 newer_profile(float4 value : COLOR0) : SV_Target { return value; }
