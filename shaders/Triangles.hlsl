cbuffer TransformConstants : register(b0)
{
    row_major float4x4 model;
    row_major float4x4 view;
    row_major float4x4 projection;
    row_major float4x4 normalMatrix;
};

Texture2D<float4> baseColorTexture : register(t0);
Texture2D<float4> normalTexture : register(t1);
Texture2D<float4> metallicRoughnessTexture : register(t2);
Texture2D<float> shadowMapTexture : register(t3);
SamplerState materialSampler : register(s0);
SamplerComparisonState shadowComparisonSampler : register(s1);

cbuffer PbrConstants : register(b1)
{
    float4 baseColorFactor;
    float3 cameraWorldPosition;
    float metallicFactor;
    float3 directionalLightDirection;
    float roughnessFactor;
    float3 directionalLightColor;
    float normalScale;
    float directionalLightIntensity;
    float ambientIntensity;
    float modelHandedness;
    float pbrPadding;
};

cbuffer ShadowConstants : register(b2)
{
    row_major float4x4 lightViewProjection;
    float2 shadowTexelSize;
    float shadowReceiverBias;
    float shadowPadding;
};

static const float Pi = 3.14159265358979323846f;

struct VertexShaderInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 textureCoordinates : TEXCOORD;
    float4 tangent : TANGENT;
};

struct VertexShaderOutput
{
    float4 position : SV_Position;
    float3 worldPosition : POSITION;
    float3 worldNormal : NORMAL;
    float2 textureCoordinates : TEXCOORD;
    float4 worldTangent : TANGENT;
    float4 lightClipPosition : TEXCOORD1;
};

VertexShaderOutput VSMain(VertexShaderInput input)
{
    VertexShaderOutput output;

    const float4 worldPosition = mul(
        float4(input.position, 1.0f),
        model
    );
    const float4 viewPosition = mul(worldPosition, view);
    output.position = mul(viewPosition, projection);
    output.worldPosition = worldPosition.xyz;
    output.lightClipPosition = mul(worldPosition, lightViewProjection);
    output.worldNormal = normalize(
        mul(float4(input.normal, 0.0f), normalMatrix).xyz
    );
    output.worldTangent = float4(
        normalize(mul(float4(input.tangent.xyz, 0.0f), model).xyz),
        input.tangent.w * modelHandedness
    );
    output.textureCoordinates = input.textureCoordinates;

    return output;
}

float4 VSShadow(VertexShaderInput input) : SV_Position
{
    const float4 worldPosition = mul(
        float4(input.position, 1.0f),
        model
    );
    return mul(worldPosition, lightViewProjection);
}

float DistributionGgx(float3 normal, float3 halfVector, float roughness)
{
    const float alpha = roughness * roughness;
    const float alphaSquared = alpha * alpha;
    const float normalDotHalf = saturate(dot(normal, halfVector));
    const float normalDotHalfSquared = normalDotHalf * normalDotHalf;
    const float denominatorTerm =
        normalDotHalfSquared * (alphaSquared - 1.0f) + 1.0f;

    return alphaSquared /
        max(Pi * denominatorTerm * denominatorTerm, 1.0e-7f);
}

float GeometrySchlickGgx(float normalDotDirection, float roughness)
{
    const float directLightingRoughness = roughness + 1.0f;
    const float k =
        directLightingRoughness * directLightingRoughness / 8.0f;

    return normalDotDirection /
        max(normalDotDirection * (1.0f - k) + k, 1.0e-7f);
}

float GeometrySmith(
    float3 normal,
    float3 viewDirection,
    float3 lightDirection,
    float roughness)
{
    const float normalDotView = saturate(dot(normal, viewDirection));
    const float normalDotLight = saturate(dot(normal, lightDirection));

    return
        GeometrySchlickGgx(normalDotView, roughness) *
        GeometrySchlickGgx(normalDotLight, roughness);
}

float3 FresnelSchlick(float cosineTheta, float3 reflectanceAtNormalIncidence)
{
    return reflectanceAtNormalIncidence +
        (1.0f - reflectanceAtNormalIncidence) *
        pow(1.0f - saturate(cosineTheta), 5.0f);
}

float3 NormalizeOrFallback(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-8f
        ? value * rsqrt(lengthSquared)
        : fallback;
}

float CalculateShadowVisibility(
    float4 lightClipPosition,
    float3 normal,
    float3 lightDirection)
{
    if (lightClipPosition.w <= 0.0f)
    {
        return 1.0f;
    }

    const float3 lightNdc =
        lightClipPosition.xyz / lightClipPosition.w;
    const float2 shadowUv = float2(
        lightNdc.x * 0.5f + 0.5f,
        0.5f - lightNdc.y * 0.5f
    );
    if (lightNdc.z < 0.0f || lightNdc.z > 1.0f ||
        any(shadowUv < 0.0f) || any(shadowUv > 1.0f))
    {
        return 1.0f;
    }

    const float slopeFactor =
        1.0f - saturate(dot(normal, lightDirection));
    const float comparisonDepth =
        lightNdc.z - shadowReceiverBias * (1.0f + slopeFactor);

    float visibility = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float2 offset =
                float2((float)x, (float)y) * shadowTexelSize;
            visibility += shadowMapTexture.SampleCmpLevelZero(
                shadowComparisonSampler,
                shadowUv + offset,
                comparisonDepth
            );
        }
    }
    return visibility / 9.0f;
}

float4 EvaluateForwardPbrLinear(VertexShaderOutput input)
{
    const float4 sampledBaseColor = baseColorTexture.Sample(
        materialSampler,
        input.textureCoordinates
    );
    const float3 baseColor =
        sampledBaseColor.rgb * baseColorFactor.rgb;

    const float4 sampledMetallicRoughness =
        metallicRoughnessTexture.Sample(
            materialSampler,
            input.textureCoordinates
        );
    const float metallic = saturate(
        sampledMetallicRoughness.b * metallicFactor
    );
    const float roughness = clamp(
        sampledMetallicRoughness.g * roughnessFactor,
        0.045f,
        1.0f
    );

    float3 tangentNormal = normalTexture.Sample(
        materialSampler,
        input.textureCoordinates
    ).xyz * 2.0f - 1.0f;
    tangentNormal.xy *= normalScale;
    tangentNormal = normalize(tangentNormal);

    const float3 geometricNormal = normalize(input.worldNormal);
    float3 tangent = input.worldTangent.xyz -
        geometricNormal * dot(geometricNormal, input.worldTangent.xyz);
    tangent *= rsqrt(max(dot(tangent, tangent), 1.0e-8f));
    const float3 bitangent =
        normalize(cross(geometricNormal, tangent)) *
        input.worldTangent.w;
    const float3 normal = normalize(
        tangent * tangentNormal.x +
        bitangent * tangentNormal.y +
        geometricNormal * tangentNormal.z
    );

    const float3 viewDirection = NormalizeOrFallback(
        cameraWorldPosition - input.worldPosition,
        geometricNormal
    );
    // The stored Directional Light vector is the direction in which light
    // rays travel, so the surface-to-light vector uses its negation.
    const float3 lightDirection = NormalizeOrFallback(
        -directionalLightDirection,
        geometricNormal
    );
    const float3 halfVector = NormalizeOrFallback(
        viewDirection + lightDirection,
        geometricNormal
    );

    const float normalDotLight = saturate(dot(normal, lightDirection));
    const float normalDotView = saturate(dot(normal, viewDirection));
    const float halfDotView = saturate(dot(halfVector, viewDirection));

    const float3 dielectricF0 = float3(0.04f, 0.04f, 0.04f);
    const float3 f0 = lerp(dielectricF0, baseColor, metallic);
    const float3 fresnel = FresnelSchlick(halfDotView, f0);
    const float distribution = DistributionGgx(
        normal,
        halfVector,
        roughness
    );
    const float geometry = GeometrySmith(
        normal,
        viewDirection,
        lightDirection,
        roughness
    );

    const float3 specular =
        distribution * geometry * fresnel /
        max(4.0f * normalDotView * normalDotLight, 1.0e-5f);
    const float3 diffuseWeight = (1.0f - fresnel) * (1.0f - metallic);
    const float3 diffuse = diffuseWeight * baseColor / Pi;
    const float3 radiance =
        directionalLightColor * directionalLightIntensity;
    const float3 directLighting =
        (diffuse + specular) * radiance * normalDotLight;
    const float shadowVisibility = CalculateShadowVisibility(
        input.lightClipPosition,
        normal,
        lightDirection
    );

    // This is a small non-IBL fill term used only until the later IBL/HDR
    // milestones. It prevents unlit dielectric faces from becoming pure black.
    const float3 ambient =
        ambientIntensity * baseColor * (1.0f - metallic);
    const float3 linearColor =
        ambient + shadowVisibility * directLighting;

    return float4(
        linearColor,
        sampledBaseColor.a * baseColorFactor.a
    );
}

float4 PSMainHdr(VertexShaderOutput input) : SV_Target0
{
    return EvaluateForwardPbrLinear(input);
}
