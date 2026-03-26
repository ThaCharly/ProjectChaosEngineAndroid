#include "Recorder.hpp"
#include <iostream>
#include <stdexcept>
#include <algorithm> 
#include <cmath>     
#include <fstream> 
#include <SFML/Window/Context.hpp> // Para enganchar funciones de OpenGL

// --- DEFINICIONES DE OPENGL ---
#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_STREAM_READ 0x88E1
#define GL_READ_ONLY 0x88B8
#endif

typedef void (*glGenBuffersFunc)(GLsizei, GLuint*);
typedef void (*glBindBufferFunc)(GLenum, GLuint);
typedef void (*glBufferDataFunc)(GLenum, GLsizeiptr, const GLvoid*, GLenum);
typedef void* (*glMapBufferFunc)(GLenum, GLenum);
typedef GLboolean (*glUnmapBufferFunc)(GLenum);
typedef void (*glDeleteBuffersFunc)(GLsizei, const GLuint*);

// Punteros globales para este archivo
glGenBuffersFunc my_glGenBuffers = nullptr;
glBindBufferFunc my_glBindBuffer = nullptr;
glBufferDataFunc my_glBufferData = nullptr;
glMapBufferFunc my_glMapBuffer = nullptr;
glUnmapBufferFunc my_glUnmapBuffer = nullptr;
glDeleteBuffersFunc my_glDeleteBuffers = nullptr;

Recorder::Recorder(int width, int height, int fps, const std::string& outputFilename) 
    : width(width), height(height), fps(fps), finalFilename(outputFilename) 
{
    this->width = width + (width % 2);
    this->height = height + (height % 2);

    tempVideoFilename = "temp_video_render.mp4";
    tempAudioFilename = "temp_audio_render.wav";

    // --- GRABACIÓN UNIVERSAL POR CPU (libx264) ---
    // Usamos libx264 (H.264 por software). Corre en cualquier máquina sin importar la GPU.
    // -preset superfast: Fundamental para que la CPU no se atragante escupiendo 4K.
    // -crf 18: Calidad Constante visualmente sin pérdida (0-51, menor es mejor).
    std::string cmd = "ffmpeg -y -loglevel warning "
                      "-f rawvideo -vcodec rawvideo "
                      "-s " + std::to_string(width) + "x" + std::to_string(height) + " "
                      "-pix_fmt rgba "
                      "-r " + std::to_string(fps) + " "
                      "-i - "
                      "-vf \"vflip,format=yuv420p\" " 
                      "-c:v libx264 -preset superfast -crf 18 " 
                      "\"" + tempVideoFilename + "\""; 

    ffmpegPipe = popen(cmd.c_str(), "w");
    if (!ffmpegPipe) throw std::runtime_error("No se pudo iniciar FFmpeg.");

    audioMixBuffer.reserve(44100 * 60 * 5);

    // --- 1. CARGAMOS LAS FUNCIONES EXTENDIDAS DE OPENGL ---
    my_glGenBuffers = (glGenBuffersFunc)sf::Context::getFunction("glGenBuffers");
    my_glBindBuffer = (glBindBufferFunc)sf::Context::getFunction("glBindBuffer");
    my_glBufferData = (glBufferDataFunc)sf::Context::getFunction("glBufferData");
    my_glMapBuffer = (glMapBufferFunc)sf::Context::getFunction("glMapBuffer");
    my_glUnmapBuffer = (glUnmapBufferFunc)sf::Context::getFunction("glUnmapBuffer");
    my_glDeleteBuffers = (glDeleteBuffersFunc)sf::Context::getFunction("glDeleteBuffers");

    if (!my_glGenBuffers || !my_glBindBuffer || !my_glBufferData || !my_glMapBuffer || !my_glUnmapBuffer) {
        throw std::runtime_error("Pah, la gráfica no soporta PBOs o falló la carga de OpenGL.");
    }

    // --- 2. INICIALIZAMOS EL DOBLE BUFFER (PING-PONG) ---
    size_t dataSize = this->width * this->height * 4;
    my_glGenBuffers(2, pbo);
    
    my_glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[0]);
    my_glBufferData(GL_PIXEL_PACK_BUFFER, dataSize, nullptr, GL_STREAM_READ);
    
    my_glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[1]);
    my_glBufferData(GL_PIXEL_PACK_BUFFER, dataSize, nullptr, GL_STREAM_READ);
    
    my_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    
    isWorkerRunning = true;
    workerThread = std::thread(&Recorder::workerLoop, this);

    std::cout << "[REC] Grabando video 4K ASÍNCRONO por software (libx264) en: " << tempVideoFilename << std::endl;
}

Recorder::~Recorder() {
    stop(); 
    if (my_glDeleteBuffers) {
        my_glDeleteBuffers(2, pbo);
    }
}

void Recorder::addFrame(const sf::Texture& texture) {
    if (!ffmpegPipe || !isRecording) return;
    currentFrame++;
    
    size_t dataSize = width * height * 4;

    // 1. Forzamos a SFML a vincular su textura en la máquina de estados de OpenGL
    sf::Texture::bind(&texture);

    // 2. TRANSFERENCIA ASÍNCRONA (VRAM -> PBO)
    // Le ordenamos al controlador DMA de la GPU que empiece a copiar la textura.
    // Esto NO bloquea la CPU, retorna instantáneamente.
    my_glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[pboIndex]);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);

    // 3. LEER EL FRAME ANTERIOR (PBO -> RAM)
    if (!firstFrame) {
        my_glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[nextPboIndex]);
        
        // Mapeamos la memoria del PBO que ya terminó de transferirse
        GLubyte* ptr = (GLubyte*)my_glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);

        if (ptr) {
            // Acá sí hacemos la copia a RAM, pero la info ya viajó por el PCIe
            std::vector<sf::Uint8> buffer(ptr, ptr + dataSize);
            my_glUnmapBuffer(GL_PIXEL_PACK_BUFFER);

            // Lo mandamos al hilo esclavo de FFmpeg con BACKPRESSURE
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                // Si la cola llega al máximo, pausamos la simulación hasta que FFmpeg libere espacio
                queueSpaceCV.wait(lock, [this] { return frameQueue.size() < MAX_QUEUE_SIZE; });
                frameQueue.push(std::move(buffer));
            }
            queueCV.notify_one();
        }
    } else {
        // Sacrificamos el primerísimo frame visual porque el PBO "next" todavía tiene basura
        firstFrame = false; 
    }

    // 4. LIMPIEZA
    my_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    sf::Texture::bind(nullptr);

    // 5. CAMBIO DE ROLES (Ping-Pong)
    pboIndex = (pboIndex + 1) % 2;
    nextPboIndex = (pboIndex + 1) % 2;
}

// ... EL RESTO QUEDA IGUAL (workerLoop, stop, addAudioEvent) ...

void Recorder::workerLoop() {
    while (true) {
        std::vector<sf::Uint8> currentFrameData; 
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCV.wait(lock, [this] { return !frameQueue.empty() || !isWorkerRunning; });
            
            if (frameQueue.empty() && !isWorkerRunning) break;

            currentFrameData = std::move(frameQueue.front());
            frameQueue.pop();
        }
        
        // ¡AVISAMOS AL HILO PRINCIPAL QUE HAY LUGAR EN LA RAM!
        queueSpaceCV.notify_one(); 

        // Leemos directo de la memoria contigua del vector para escupirlo a FFmpeg
        if (ffmpegPipe) {
            fwrite(currentFrameData.data(), 1, width * height * 4, ffmpegPipe); 
        }
    }
}

void Recorder::stop() {
    if (isFinished) return;
    isFinished = true;
    isRecording = false;

    // --- FRENAR EL HILO LIMPIAMENTE ---
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        isWorkerRunning = false;
    }
    queueCV.notify_one();
    if (workerThread.joinable()) {
        std::cout << "[REC] Esperando a que FFmpeg termine de digerir la cola de frames..." << std::endl;
        workerThread.join();
    }

    if (ffmpegPipe) {
        pclose(ffmpegPipe);
        ffmpegPipe = nullptr;
    }

    // (El resto del método stop() del Audio Mix y Fusión dejalo igualito a como lo tenés)
    if (!audioMixBuffer.empty()) {
        std::cout << "[REC] Procesando audio (Normalizando)..." << std::endl;
        float maxPeak = 0.0f;
        for (float s : audioMixBuffer) {
            if (std::abs(s) > maxPeak) maxPeak = std::abs(s);
        }

        float gain = 1.0f;
        if (maxPeak > 32000.0f) {
            gain = 32000.0f / maxPeak;
        } else if (maxPeak > 0.0f && maxPeak < 10000.0f) {
            gain = 25000.0f / maxPeak;
        }

        std::vector<sf::Int16> finalSamples;
        finalSamples.reserve(audioMixBuffer.size() * 2);

        for (float sample : audioMixBuffer) {
            float normalizedSample = sample * gain;
            if (normalizedSample > 32767.0f) normalizedSample = 32767.0f;
            if (normalizedSample < -32768.0f) normalizedSample = -32768.0f;
            sf::Int16 s = static_cast<sf::Int16>(normalizedSample);
            finalSamples.push_back(s); 
            finalSamples.push_back(s); 
        }

        sf::OutputSoundFile audioFile;
        if (audioFile.openFromFile(tempAudioFilename, 44100, 2)) { 
            audioFile.write(finalSamples.data(), finalSamples.size());
            audioFile.close(); 
        }
    }

    std::cout << "[REC] Iniciando fusion final..." << std::endl;
    std::string mergeCmd = "ffmpeg -y -loglevel error -i " + tempVideoFilename + " -i " + tempAudioFilename + 
                           " -c:v copy -c:a aac -b:a 192k -shortest " + finalFilename;
    int result = system(mergeCmd.c_str());

    if (result == 0) {
        std::cout << "[REC] EXITO TOTAL: " << finalFilename << std::endl;
        remove(tempVideoFilename.c_str());
        remove(tempAudioFilename.c_str());
    } else {
        std::cerr << "[REC] Error en la fusion de FFmpeg." << std::endl;
    }
}

void Recorder::addAudioEvent(const sf::Int16* samples, std::size_t sampleCount, float volume) {
    if (!isRecording) return;

    size_t startIndex = (size_t)((double)currentFrame / fps * sampleRate);
    size_t requiredSize = startIndex + sampleCount;
    
    if (audioMixBuffer.size() < requiredSize) {
        audioMixBuffer.resize(requiredSize, 0.0f); 
    }
    
    float volFactor = volume / 100.0f;
    for (size_t i = 0; i < sampleCount; ++i) {
        audioMixBuffer[startIndex + i] += (float)samples[i] * volFactor;
    }
}