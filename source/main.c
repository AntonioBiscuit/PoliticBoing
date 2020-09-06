#include <citro2d.h>
#include <citro3d.h>
#include <3ds.h>
#include <opusfile.h>

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#define MAX_SPRITES   1024
#define SCREEN_WIDTH  400
#define SCREEN_HEIGHT 240

float drawtime = 0;
float fps = 0;

// Array pour la fonction Opus
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

// ---- DEFINITIONS ----

static const char *PATH = "romfs:/audio/Somber.opus";  // Path to Opus file to play

static const int SAMPLE_RATE = 48000;            // Opus is fixed at 48kHz
static const int SAMPLES_PER_BUF = SAMPLE_RATE * 120 / 1000;  // 120ms buffer
static const int CHANNELS_PER_SAMPLE = 2;        // We ask libopusfile for
                                                 // stereo output; it will down
                                                 // -mix for us as necessary.

static const int THREAD_AFFINITY = -1;           // Execute thread on any core
static const int THREAD_STACK_SZ = 32 * 1024;    // 32kB stack for audio thread

static const size_t WAVEBUF_SIZE = SAMPLES_PER_BUF * CHANNELS_PER_SAMPLE
* sizeof(int16_t);                           // Size of NDSP wavebufs

// ---- END DEFINITIONS ----

ndspWaveBuf s_waveBufs[3];
int16_t *s_audioBuffer = NULL;

LightEvent s_event;
volatile bool s_quit = false;  // Quit flag

// ---- HELPER FUNCTIONS ----

// Retrieve strings for libopusfile errors
// Sourced from David Gow's example code: https://davidgow.net/files/opusal.cpp
const char *opusStrError(int error)
{
    switch(error) 
    {
        case OP_FALSE:
            return "OP_FALSE: A request did not succeed.";
        case OP_HOLE:
            return "OP_HOLE: There was a hole in the page sequence numbers.";
        case OP_EREAD:
            return "OP_EREAD: An underlying read, seek or tell operation "
                   "failed.";
        case OP_EFAULT:
            return "OP_EFAULT: A NULL pointer was passed where none was "
                   "expected, or an internal library error was encountered.";
        case OP_EIMPL:
            return "OP_EIMPL: The stream used a feature which is not "
                   "implemented.";
        case OP_EINVAL:
            return "OP_EINVAL: One or more parameters to a function were "
                   "invalid.";
        case OP_ENOTFORMAT:
            return "OP_ENOTFORMAT: This is not a valid Ogg Opus stream.";
        case OP_EBADHEADER:
            return "OP_EBADHEADER: A required header packet was not properly "
                   "formatted.";
        case OP_EVERSION:
            return "OP_EVERSION: The ID header contained an unrecognised "
                   "version number.";
        case OP_EBADPACKET:
            return "OP_EBADPACKET: An audio packet failed to decode properly.";
        case OP_EBADLINK:
            return "OP_EBADLINK: We failed to find data we had seen before or "
                   "the stream was sufficiently corrupt that seeking is "
                   "impossible.";
        case OP_ENOSEEK:
            return "OP_ENOSEEK: An operation that requires seeking was "
                   "requested on an unseekable stream.";
        case OP_EBADTIMESTAMP:
            return "OP_EBADTIMESTAMP: The first or last granule position of a "
                   "link failed basic validity checks.";
        default:
            return "Unknown error.";
    }
}

// Pause until user presses a button
void waitForInput(void) 
{
    printf("Press any button to exit...\n");
    while(aptMainLoop())
    {
        gspWaitForVBlank();
        gfxSwapBuffers();
        hidScanInput();

        if(hidKeysDown())
            break;
    }
}

// ---- END HELPER FUNCTIONS ----

// Audio initialisation code
// This sets up NDSP and our primary audio buffer
bool audioInit(void) 
{
    // Setup NDSP
    ndspChnReset(0);
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(0, SAMPLE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);

    // Allocate audio buffer
    const size_t bufferSize = WAVEBUF_SIZE * ARRAY_SIZE(s_waveBufs);
    s_audioBuffer = (int16_t *)linearAlloc(bufferSize);
    if(!s_audioBuffer) 
    {
        printf("Failed to allocate audio buffer\n");
        return false;
    }

    // Setup waveBufs for NDSP
    memset(&s_waveBufs, 0, sizeof(s_waveBufs));
    int16_t *buffer = s_audioBuffer;

    for(size_t i = 0; i < ARRAY_SIZE(s_waveBufs); ++i) 
    {
        s_waveBufs[i].data_vaddr = buffer;
        s_waveBufs[i].status     = NDSP_WBUF_DONE;

        buffer += WAVEBUF_SIZE / sizeof(buffer[0]);
    }

    return true;
}

// Audio de-initialisation code
// Stops playback and frees the primary audio buffer
void audioExit(void) 
{
    ndspChnReset(0);
    linearFree(s_audioBuffer);
}

// Main audio decoding logic
// This function pulls and decodes audio samples from opusFile_ to fill waveBuf_
bool fillBuffer(OggOpusFile *opusFile_, ndspWaveBuf *waveBuf_) 
{
    #ifdef DEBUG
    // Setup timer for performance stats
    TickCounter timer;
    osTickCounterStart(&timer);
    #endif  // DEBUG

    // Decode samples until our waveBuf is full
    int totalSamples = 0;
    while(totalSamples < SAMPLES_PER_BUF) 
    {
        int16_t *buffer = waveBuf_->data_pcm16 + (totalSamples *
            CHANNELS_PER_SAMPLE);
        const size_t bufferSize = (SAMPLES_PER_BUF - totalSamples) *
            CHANNELS_PER_SAMPLE;

        // Decode bufferSize samples from opusFile_ into buffer,
        // storing the number of samples that were decoded (or error)
        const int samples = op_read_stereo(opusFile_, buffer, bufferSize);
        if(samples <= 0) {
            if(samples == 0) break;  // No error here

            printf("op_read_stereo: error %d (%s)", samples,
                   opusStrError(samples));
            break;
        }
        
        totalSamples += samples;
    }

    // If no samples were read in the last decode cycle, we're done
    if(totalSamples == 0) 
    {
        printf("Playback complete, press Start to exit\n");
        return false;
    }

    // Pass samples to NDSP
    waveBuf_->nsamples = totalSamples;
    ndspChnWaveBufAdd(0, waveBuf_);
    DSP_FlushDataCache(waveBuf_->data_pcm16,
        totalSamples * CHANNELS_PER_SAMPLE * sizeof(int16_t));

    #ifdef DEBUG
    // Print timing info
    osTickCounterUpdate(&timer);
    printf("fillBuffer %lfms in %lfms\n", totalSamples * 1000.0 / SAMPLE_RATE,
           osTickCounterRead(&timer));
    #endif  // DEBUG

    return true;
}

// NDSP audio frame callback
// This signals the audioThread to decode more things
// once NDSP has played a sound frame, meaning that there should be
// one or more available waveBufs to fill with more data.
void audioCallback(void *const nul_) 
{
    (void)nul_;  // Unused

    if(s_quit) // Quit flag
    { 
        return;
    }
    
    LightEvent_Signal(&s_event);
}

// Audio thread
// This handles calling the decoder function to fill NDSP buffers as necessary
void audioThread(void *const opusFile_) {
    OggOpusFile *const opusFile = (OggOpusFile *)opusFile_;

    while(!s_quit) // Whilst the quit flag is unset,
                      // search our waveBufs and fill any that aren't currently
                      // queued for playback (i.e, those that are 'done')
    {  
        for(size_t i = 0; i < ARRAY_SIZE(s_waveBufs); ++i) 
        {
            if(s_waveBufs[i].status != NDSP_WBUF_DONE) 
            {
                continue;
            }
            
            if(!fillBuffer(opusFile, &s_waveBufs[i])) // Playback complete
            {   
                return;
            }
        }

        // Wait for a signal that we're needed again before continuing,
        // so that we can yield to other things that want to run
        // (Note that the 3DS uses cooperative threading)
        LightEvent_Wait(&s_event);
    }
}

// END OF AUDIO RELATED STUFF


// Simple sprite struct
typedef struct
{
	C2D_Sprite spr;
	float dx, dy; // velocity
} Sprite;

static C2D_SpriteSheet spriteSheet;
static Sprite sprites[MAX_SPRITES];
// Nombre de sprites a afficher (256 par defaut)
static size_t numSprites = 256;

//---------------------------------------------------------------------------------
static void initSprites() {
//---------------------------------------------------------------------------------
	size_t numImages = C2D_SpriteSheetCount(spriteSheet);
	srand(time(NULL));

	for (size_t i = 0; i < MAX_SPRITES; i++)
	{
		Sprite* sprite = &sprites[i];

		// Random image, position, rotation and speed
		C2D_SpriteFromSheet(&sprite->spr, spriteSheet, rand() % numImages);
		C2D_SpriteSetCenter(&sprite->spr, 0.5f, 0.5f);
		C2D_SpriteSetPos(&sprite->spr, rand() % SCREEN_WIDTH, rand() % SCREEN_HEIGHT);
		C2D_SpriteSetRotation(&sprite->spr, C3D_Angle(rand()/(float)RAND_MAX));
		sprite->dx = rand()*4.0f/RAND_MAX - 2.0f;
		sprite->dy = rand()*4.0f/RAND_MAX - 2.0f;
	}
}

//---------------------------------------------------------------------------------
static void moveSprites() {
//---------------------------------------------------------------------------------
	for (size_t i = 0; i < numSprites; i++)
	{
		Sprite* sprite = &sprites[i];
		C2D_SpriteMove(&sprite->spr, sprite->dx, sprite->dy);
		C2D_SpriteRotateDegrees(&sprite->spr, 1.0f);

		// Check for collision with the screen boundaries
		if ((sprite->spr.params.pos.x < sprite->spr.params.pos.w / 2.0f && sprite->dx < 0.0f) ||
			(sprite->spr.params.pos.x > (SCREEN_WIDTH-(sprite->spr.params.pos.w / 2.0f)) && sprite->dx > 0.0f))
        {
            sprite->dx = -sprite->dx;
        }
			

		if ((sprite->spr.params.pos.y < sprite->spr.params.pos.h / 2.0f && sprite->dy < 0.0f) ||
			(sprite->spr.params.pos.y > (SCREEN_HEIGHT-(sprite->spr.params.pos.h / 2.0f)) && sprite->dy > 0.0f))
        {
            sprite->dy = -sprite->dy;
        }
			
	}
}

//---------------------------------------------------------------------------------
int main(int argc, char* argv[]) {
//---------------------------------------------------------------------------------
	// Init libs
	romfsInit();
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();
	consoleInit(GFX_BOTTOM, NULL);
    ndspInit();

    // Enable N3DS 804MHz operation, where available
    osSetSpeedupEnable(true);
    
    // Setup LightEvent for synchronisation of audioThread
    LightEvent_Init(&s_event, RESET_ONESHOT);


    // AUDIO DEBUG OUTPUT ----------
    /*
    printf("Opus audio streaming example\n"
           "thejsa and mtheall, May 2020\n"
           "Press START to exit\n"
           "\n"
           "Using %d waveBufs, each of length %d bytes\n"
           "    (%d samples; %lf ms @ %d Hz)\n"
           "\n"
           "Loading audio data from path: %s\n"
           "\n",
           ARRAY_SIZE(s_waveBufs), WAVEBUF_SIZE, SAMPLES_PER_BUF,
           SAMPLES_PER_BUF * 1000.0 / SAMPLE_RATE, SAMPLE_RATE,
           PATH);
    */
   // ------------------------------


    // Open the Opus audio file
    int error = 0;
    OggOpusFile *opusFile = op_open_file(PATH, &error);
    if(error) 
    {
        printf("Failed to open file: error %d (%s)\n", error,
               opusStrError(error));
        waitForInput();
    }

    // Attempt audioInit
    if(!audioInit()) 
    {
        printf("Failed to initialise audio\n");
        waitForInput();

        gfxExit();
        ndspExit();
        romfsExit();
        return EXIT_FAILURE;
    }

    // Set the ndsp sound frame callback which signals our audioThread
    ndspSetCallback(audioCallback, NULL);

    // Spawn audio thread
    // Set the thread priority to the main thread's priority ...
    int32_t priority = 0x3F;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    // ... then subtract 1, as lower number => higher actual priority ...
    // priority -= 1;
    // ... finally, clamp it between 0x18 and 0x3F to guarantee that it's valid.
    priority = priority < 0x18 ? 0x18 : priority;
    priority = priority > 0x3F ? 0x3F : priority;

    // Start the thread, passing our opusFile as an argument.
    const Thread threadId = threadCreate(audioThread, opusFile,
                                         THREAD_STACK_SZ, priority,
                                         THREAD_AFFINITY, false);
    printf("Created audio thread %p\n", threadId);

	// Create screens
	C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);

	// Load graphics
	spriteSheet = C2D_SpriteSheetLoad("romfs:/gfx/sprites.t3x");
	if (!spriteSheet) svcBreak(USERBREAK_PANIC);

	// Initialize sprites
	initSprites();

	printf("\x1b[8;1HPress Up pour plus de TRUMP POWER");
	printf("\x1b[10;1HPress Down pour calmer Mr Trump");
    printf("\x1b[15;1HCode by AntonioBiscuit");
    

	// Main loop
	while (aptMainLoop())
	{
		hidScanInput();
		// Respond to user input
		u32 kDown = hidKeysDown();
		if (kDown & KEY_START)
        {
            break; // break in order to return to hbmenu
        }

		u32 kHeld = hidKeysHeld();
		if ((kHeld & KEY_UP) && numSprites < MAX_SPRITES)
        {
            numSprites++;
        }
			
		if ((kHeld & KEY_DOWN) && numSprites > 1)
        {
            numSprites--;
        }

		moveSprites();

		// Render the scene
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_TargetClear(top, C2D_Color32f(0.0f, 0.0f, 0.0f, 1.0f));
		C2D_SceneBegin(top);
		for (size_t i = 0; i < numSprites; i ++)
        {
            C2D_DrawSprite(&sprites[i].spr);
        }

		// Print DEBUG info
        // Actualiser le compteur de FPS et l'utilisation CPU/GPU
		drawtime = C3D_GetDrawingTime();
		fps = 1000 / drawtime;

        printf("\x1b[1;1HSprites: %zu/%u\x1b[K", numSprites, MAX_SPRITES);
		printf("\x1b[2;1HCPU:     %6.2f%%\x1b[K", C3D_GetProcessingTime()*6.0f);
		printf("\x1b[3;1HGPU:     %6.2f%%\x1b[K", C3D_GetDrawingTime()*6.0f);
		printf("\x1b[4;1HCmdBuf:  %6.2f%%\x1b[K", C3D_GetCmdBufUsage()*100.0f);
        
        printf("\x1b[20;1HFPS:     %6.2f\x1b[K", fps);
		printf("\x1b[22;1HDraw ms: %6.2f\x1b[K", drawtime);

        C3D_FrameEnd(0);
	}
        
    // Signal audio thread to quit
    s_quit = true;
    LightEvent_Signal(&s_event);

    // Free the audio thread
    threadJoin(threadId, UINT64_MAX);
    threadFree(threadId);

    // Cleanup audio things and de-init platform features
    audioExit();
    ndspExit();
    op_free(opusFile);

	// Delete graphics
	C2D_SpriteSheetFree(spriteSheet);

	// Deinit libs
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	romfsExit();
	
    return 0;
}
