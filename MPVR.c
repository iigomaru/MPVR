// very rough but it works, proof of concept embedding mpv into a openvr overlay

// System headers for any extra stuff we need.
#include <stdbool.h>

// Include CNFG (rawdraw) for generating a window and/or OpenGL context.
#define CNFG_IMPLEMENTATION
#define CNFGOGL
#include "rawdraw_sf.h"

// Include OpenVR header so we can interact with VR stuff.
#undef EXTERN_C
#include "openvr_capi.h"

#include "include/client.h"
#include "include/render_gl.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define TAU 6.28318530718

/* The settings for the positioning of the overlay relative to the right controller */
#define XANGLE 45.0  /* The default value is 45.0  */
#define YANGLE 90.0  /* The default value is 90.0  */
#define ZANGLE -90.0 /* The default value is -90.0 */
#define TRANS1 0.10  /* The default value is 0.05  */
#define TRANS2 -0.05 /* The default value is -0.05 */
#define TRANS3 0.24  /* The default value is 0.24  */

static void die(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

static void *get_proc_address_mpv(void *fn_ctx, const char *name)
{
    return CNFGGetProcAddress(name);
}

static HmdMatrix34_t 
EulerToHmdMatrix34_t(double XAngle, double YAngle, double ZAngle, double Trans1, double Trans2, double Trans3)
{
	HmdMatrix34_t transform = { 0 };
	float X = -XAngle*TAU/360;
	float Y = -YAngle*TAU/360;
	float Z = -ZAngle*TAU/360;
	float cx = cosf(X);
	float sx = sinf(X);
	float cy = cosf(Y);
	float sy = sinf(Y);
	float cz = cosf(Z);
	float sz = sinf(Z);

	transform.m[0][0] = cy*cz;
	transform.m[1][0] = (sx*sy*cz)-(cx*sz);
	transform.m[2][0] = (cx*sy*cz)+(sx*sz);

	transform.m[0][1] = -(cy*sz);
	transform.m[1][1] = -((sx*sy*sz)+(cx*cz));
	transform.m[2][1] = -((cx*sy*sz)-(sx*cz));

	transform.m[0][2] = -sy;
	transform.m[1][2] = sx*cy;
	transform.m[2][2] = cx*cy;

	transform.m[0][3] = 0+Trans1;
	transform.m[1][3] = 0+Trans2;
	transform.m[2][3] = 0+Trans3;
	return transform;
}

// OpenVR Doesn't define these for some reason (I don't remmeber why) so we define the functions here. They are copy-pasted from the bottom of openvr_capi.h
intptr_t VR_InitInternal( EVRInitError *peError, EVRApplicationType eType );
void VR_ShutdownInternal();
bool VR_IsHmdPresent();
intptr_t VR_GetGenericInterface( const char *pchInterfaceVersion, EVRInitError *peError );
bool VR_IsRuntimeInstalled();
const char * VR_GetVRInitErrorAsSymbol( EVRInitError error );
const char * VR_GetVRInitErrorAsEnglishDescription( EVRInitError error );

// These are functions that rawdraw calls back into.
void HandleKey( int keycode, int bDown ) { }
void HandleButton( int x, int y, int button, int bDown ) { }
void HandleMotion( int x, int y, int mask ) { }
void HandleDestroy() { }

// This function was copy-pasted from cnovr.
void * CNOVRGetOpenVRFunctionTable( const char * interfacename )
{
	EVRInitError e;
	char fnTableName[128];
	int result1 = snprintf( fnTableName, 128, "FnTable:%s", interfacename );
	void * ret = (void *)VR_GetGenericInterface( fnTableName, &e );
	printf( "Getting System FnTable: %s = %p (%d)\n", fnTableName, ret, e );
	if( !ret )
	{
		exit( 1 );
	}
	return ret;
}

// These are interfaces into OpenVR, they are basically function call tables.
struct VR_IVRSystem_FnTable * oSystem;
struct VR_IVROverlay_FnTable * oOverlay;

// The OpenVR Overlay handle.
VROverlayHandle_t overlayID;

// Was the overlay assocated or not?
int overlayAssociated;

// The width/height of the overlay.
#define WIDTH 854
#define HEIGHT 480


int main(int argc, char *argv[])
{
    if (argc != 2)
        die("pass a single media file as argument");

    mpv_handle *mpv = mpv_create();
    if (!mpv)
        die("context init failed");

    // Some minor options can only be set before mpv_initialize().
    if (mpv_initialize(mpv) < 0)
        die("mpv init failed");

    mpv_request_log_messages(mpv, "debug");


	// Create the window, needed for making an OpenGL context, but also
	// gives us a framebuffer we can draw into.  Minus signs in front of 
	// width/height hint to rawdrawthat we want a hidden window.
	CNFGSetup( "MPVR", -WIDTH, -HEIGHT );

	mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_OPENGL},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &(mpv_opengl_init_params){
            .get_proc_address = get_proc_address_mpv,
        }},
        // Tell libmpv that you will call mpv_render_context_update() on render
        // context update callbacks, and that you will _not_ block on the core
        // ever (see <libmpv/render.h> "Threading" section for what libmpv
        // functions you can call at all when this is active).
        // In particular, this means you must call e.g. mpv_command_async()
        // instead of mpv_command().
        // If you want to use synchronous calls, either make them on a separate
        // thread, or remove the option below (this will disable features like
        // DR and is not recommended anyway).
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &(int){1}},
        {0}
    };

	// This makes mpv use the currently set GL context. It will use the callback
    // (passed via params) to resolve GL builtin functions, as well as extensions.
    mpv_render_context *mpv_gl;
    if (mpv_render_context_create(&mpv_gl, mpv, params) < 0)
        die("failed to initialize mpv GL context");

	// Play this file.
    const char *cmd[] = {"loadfile", argv[1], NULL};
    mpv_command_async(mpv, 0, cmd);

	// We put this in a codeblock because it's logically together.
	// no reason to keep the token around.
	{
		EVRInitError ierr;
		uint32_t token = VR_InitInternal( &ierr, EVRApplicationType_VRApplication_Overlay );
		if( !token )
		{
			printf( "Error!!!! Could not initialize OpenVR\n" );
			return -5;
		}

		// Get the system and overlay interfaces.  We pass in the version of these
		// interfaces that we wish to use, in case the runtime is newer, we can still
		// get the interfaces we expect.
		oSystem = CNOVRGetOpenVRFunctionTable( IVRSystem_Version );
		oOverlay = CNOVRGetOpenVRFunctionTable( IVROverlay_Version );
	}

	{
		// Generate the overlay.
		oOverlay->CreateOverlay( "MPVR", "MPV VR PLAYER", &overlayID );
		oOverlay->SetOverlayWidthInMeters( overlayID, .3);
		oOverlay->SetOverlayColor( overlayID, 1., .8, .7 );

		// Control texture bounds to control the way the texture is mapped to the overlay.
		VRTextureBounds_t bounds;
		bounds.uMin = 1;
		bounds.uMax = 0;
		bounds.vMin = 0;
		bounds.vMax = 1;
		oOverlay->SetOverlayTextureBounds( overlayID, &bounds );
	}

	// Actually show the overlay.
	oOverlay->ShowOverlay( overlayID );


	GLuint overlaytexture;
	{
		// Initialize the texture with junk data.
		uint8_t * myjunkdata = malloc( 854 * 854 * 4 );
		int x, y;
		for( y = 0; y < 854; y++ )
		for( x = 0; x < 854; x++ )
		{
			myjunkdata[ ( x + y * 854 ) * 4 + 0 ] = x * 2;
			myjunkdata[ ( x + y * 854 ) * 4 + 1 ] = y * 2;
			myjunkdata[ ( x + y * 854 ) * 4 + 2 ] = 0;
			myjunkdata[ ( x + y * 854 ) * 4 + 3 ] = 255;
		}
		
		// We aren't doing it, but we could write directly into the overlay.
		//err = oOverlay->SetOverlayRaw( overlayID, myjunkdata, 128, 128, 4 );
		
		// Generate the texture.
		glGenTextures( 1, &overlaytexture );
		glBindTexture( GL_TEXTURE_2D, overlaytexture );

		// It is required to setup the min and mag filter of the texture.
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		
		// Load the texture with our dummy data.  Optionally we could pass 0 in where we are
		// passing in myjunkdata. That would allocate the RAM on the GPU but not do anything with it.
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, WIDTH, HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, myjunkdata );
	}

	/* 
	* Transform for the right hand pos that puts the text somewhere reasonable,
	* the euler angles, and transpose values are set using 
	* the config.h include file.
	*/
	HmdMatrix34_t rightHandTransform = EulerToHmdMatrix34_t(XANGLE, YANGLE, ZANGLE, TRANS1, TRANS2, TRANS3);

	int framenumber;

	while( true )
	{
		int redraw = 0;
		uint64_t flags = mpv_render_context_update(mpv_gl);
		if (flags & MPV_RENDER_UPDATE_FRAME)
		{
			redraw = 1;
		}
		
		CNFGBGColor = 0x00000000; //Black Transparent Background
		CNFGClearFrame();
		
		// Process any window events and call callbacks.
		CNFGHandleInput();

		if (redraw) {
            short w, h;
            CNFGGetDimensions(&w, &h);
            mpv_render_param params[] = {
                // Specify the default framebuffer (0) as target. This will
                // render onto the entire screen. If you want to show the video
                // in a smaller rectangle or apply fancy transformations, you'll
                // need to render into a separate FBO and draw it manually.
                {MPV_RENDER_PARAM_OPENGL_FBO, &(mpv_opengl_fbo){
                    .fbo = 0,
                    .w = w,
                    .h = h,
                }},
                // Flip rendering (needed due to flipped GL coordinate system).
                {MPV_RENDER_PARAM_FLIP_Y, &(int){1}},
                {0}
            };
            // See render_gl.h on what OpenGL environment mpv expects, and
            // other API details.
            mpv_render_context_render(mpv_gl, params);
        } 

		if( !overlayAssociated)
		{
			TrackedDeviceIndex_t index;
			index = oSystem->GetTrackedDeviceIndexForControllerRole( ETrackedControllerRole_TrackedControllerRole_RightHand );
			if( index == k_unTrackedDeviceIndexInvalid || index == k_unTrackedDeviceIndex_Hmd )
			{
				printf( "Couldn't find your controller to attach our overlay to (%d)\n", index );
			}
			else
			{
				/* We have a ETrackedControllerRole_TrackedControllerRole_RightHand. Associate it. */
				EVROverlayError err;

				/* Apply the transform and attach the overlay to that tracked device object. */
				err = oOverlay->SetOverlayTransformTrackedDeviceRelative( overlayID, index, &rightHandTransform );

				/* Notify the terminal that this was associated. */
				printf( "Successfully associated your overlay window to the tracked device (%d %d %08x).\n",
					 err, index, overlayID );

				overlayAssociated = true;
			}
		}

		// Bind the texture we will be sending to OpenVR.
		glBindTexture( GL_TEXTURE_2D, overlaytexture );
		
		// Copy the current framebuffer into that texture.
		glCopyTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, WIDTH, HEIGHT, 0 );

		// Setup a Texture_t object to send in the texture.
		struct Texture_t tex;
		tex.eColorSpace = EColorSpace_ColorSpace_Auto;
		tex.eType = ETextureType_TextureType_OpenGL;
		tex.handle = (void*)(intptr_t)overlaytexture;

		// Send texture into OpenVR as the overlay.
		oOverlay->SetOverlayTexture( overlayID, &tex );

		// We have to process through texture events.
		struct VREvent_t nEvent;
		if( overlayAssociated )
		{
			oOverlay->PollNextOverlayEvent( overlayID, &nEvent, 0xffffff );
		}

		// Display the image and wait for time to display next frame.
		CNFGSwapBuffers();

		framenumber++;
		
		// Don't go at 1,000+ FPS.
		Sleep( 50 );
	}

    return 0;
}