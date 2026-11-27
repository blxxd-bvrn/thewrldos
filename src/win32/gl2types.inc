/* OpenGL 2+ state types (gl2.inc), included by gl.c after glctx_t */
#define NATTR 16
#define NVAO 256
typedef struct { bool used; uint8_t *data; uint32_t size; GLenum usage; bool mapped; } glbuf_t;
typedef struct { bool on, norm, integer; GLint size; GLenum type; GLsizei stride; uintptr_t ptr; GLuint buf; GLuint divisor; } vattr_t;
typedef struct { bool used; vattr_t a[NATTR]; GLuint ebo; } glvao_t;
struct gl2 {
    glbuf_t *buf; int nbuf;                  /* buffer objects by name; 0 is unused */
    glvao_t vao[NVAO]; GLuint vao_bound;     /* 0 is the default vertex array */
    GLuint abo;                              /* GL_ARRAY_BUFFER */
    float cur[NATTR][4];                     /* glVertexAttrib values for disabled arrays */
    GLuint unit_tex[16];                     /* glBindTexture per texture unit */
    float blend_color[4];
    GLenum bsrc, bdst, bsrc_a, bdst_a, beq, beq_a;
    GLenum depth_func;
    bool cmask[4];
    struct glsl_state *sl;                   /* glsl.inc */
};
