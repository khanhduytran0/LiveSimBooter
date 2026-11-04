#include <metal_stdlib>
using namespace metal;
struct Vertex { float4 position; float4 color; };
struct VertexOut { float4 position [[position]]; float4 color; };
vertex VertexOut vertex_main(uint vertex_id [[vertex_id]], const device Vertex *vertices [[buffer(0)]]) {
    VertexOut out; out.position = vertices[vertex_id].position; out.color = vertices[vertex_id].color; return out;
}
fragment float4 fragment_main(VertexOut in [[stage_in]]) { return in.color; }
