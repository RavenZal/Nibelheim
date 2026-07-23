Texture2D<float4> hdrTexture : register(t0);
SamplerState linearClampSampler : register(s0);

cbuffer ToneMappingConstants : register(b0)
{
    float exposure;
    float3 toneMappingPadding;
};

struct FullscreenVertexOutput
{
    float4 position : SV_Position;
    float2 textureCoordinates : TEXCOORD0;
};

FullscreenVertexOutput VSMain(uint vertexId : SV_VertexID)
{
    FullscreenVertexOutput output;

    // Three generated vertices cover the viewport without a vertex buffer.
    // The oversized UV range is clipped together with the oversized triangle.
    const float2 textureCoordinates = float2(
        (vertexId << 1) & 2,
        vertexId & 2
    );
    output.textureCoordinates = textureCoordinates;
    output.position = float4(
        textureCoordinates * float2(2.0f, -2.0f) +
            float2(-1.0f, 1.0f),
        0.0f,
        1.0f
    );
    return output;
}

float3 AcesFitted(float3 linearColor)
{
    const float3 numerator =
        linearColor * (2.51f * linearColor + 0.03f);
    const float3 denominator =
        linearColor * (2.43f * linearColor + 0.59f) + 0.14f;
    return saturate(numerator / denominator);
}

float3 LinearToSrgb(float3 linearColor)
{
    const float3 lower = linearColor * 12.92f;
    const float3 upper =
        1.055f * pow(max(linearColor, 0.0f), 1.0f / 2.4f) - 0.055f;
    return lerp(upper, lower, linearColor <= 0.0031308f);
}

float4 PSMain(FullscreenVertexOutput input) : SV_Target0
{
    const float3 hdrColor = max(
        hdrTexture.Sample(linearClampSampler, input.textureCoordinates).rgb,
        0.0f
    );
    const float3 exposedColor = hdrColor * max(exposure, 0.0f);
    const float3 toneMappedColor = AcesFitted(exposedColor);
    return float4(LinearToSrgb(toneMappedColor), 1.0f);
}
