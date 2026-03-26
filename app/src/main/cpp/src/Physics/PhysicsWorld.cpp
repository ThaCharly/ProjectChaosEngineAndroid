#include "PhysicsWorld.hpp"
#include <cmath>
#include <iostream>
#include <fstream> 
#include <sstream> 
#include <SFML/System/FileInputStream.hpp>

void ChaosContactListener::BeginContact(b2Contact* contact) {
    b2Fixture* fa = contact->GetFixtureA();
    b2Fixture* fb = contact->GetFixtureB();
    b2Body* bodyA = fa->GetBody();
    b2Body* bodyB = fb->GetBody();

    bool isSensorA = fa->IsSensor();
    bool isSensorB = fb->IsSensor();

    // DETECTAR PICKUPS
    if (bodyA->GetType() == b2_dynamicBody && isSensorB) pendingPickups.push_back({bodyA, bodyB});
    else if (bodyB->GetType() == b2_dynamicBody && isSensorA) pendingPickups.push_back({bodyB, bodyA});

    // DETECTAR CHOQUE RACER vs RACER (ninguno es sensor)
    if (bodyA->GetType() == b2_dynamicBody && bodyB->GetType() == b2_dynamicBody && !isSensorA && !isSensorB) {
        pendingKills.push_back({bodyA, bodyB});
        pendingKills.push_back({bodyB, bodyA}); // Encolamos los dos lados por las dudas
    }

    if (winZoneBody) {
        if (bodyA == winZoneBody && bodyB->GetType() == b2_dynamicBody) bodiesReachedWinZone.insert(bodyB);
        else if (bodyB == winZoneBody && bodyA->GetType() == b2_dynamicBody) bodiesReachedWinZone.insert(bodyA);
    }

    if (fa->GetBody()->GetType() == b2_dynamicBody && fb->GetBody()->GetType() == b2_dynamicBody) {
        bodiesToCheck.insert(fa->GetBody());
        bodiesToCheck.insert(fb->GetBody());
    }

    // Lógica de Paredes
    b2Body* dynamicBody = nullptr;
    b2Body* wallBody = nullptr;

    // Chequeamos si es una pared fija (static) o una plataforma móvil (kinematic)
    bool isBWall = (bodyB->GetType() == b2_staticBody || bodyB->GetType() == b2_kinematicBody);
    bool isAWall = (bodyA->GetType() == b2_staticBody || bodyA->GetType() == b2_kinematicBody);

    if (bodyA->GetType() == b2_dynamicBody && isBWall) {
        dynamicBody = bodyA; wallBody = bodyB;
    } else if (bodyB->GetType() == b2_dynamicBody && isAWall) {
        dynamicBody = bodyB; wallBody = bodyA;
    }

if (dynamicBody && wallBody) {
        bodiesToCheck.insert(dynamicBody);
        wallsHit.insert(wallBody);

        // --- EXTRACCIÓN PARA PARTÍCULAS ---
        b2WorldManifold worldManifold;
        contact->GetWorldManifold(&worldManifold);
        
        CollisionEvent ev;
        ev.point = worldManifold.points[0]; // Punto de impacto
        
        // Box2D saca la normal siempre de A hacia B. 
        // Nosotros necesitamos que apunte DESDE la pared HACIA afuera.
        ev.normal = (bodyA == wallBody) ? worldManifold.normal : -worldManifold.normal; 
        ev.racer = dynamicBody;
        ev.wall = wallBody;
        
        collisionEvents.push_back(ev);
    }
}

PhysicsWorld::PhysicsWorld(float widthPixels, float heightPixels, SoundManager* soundMgr)
    : world(b2Vec2(0.0f, 0.0f)) 
{
    rng.seed(77);
    this->soundManager = soundMgr;

    contactListener.soundManager = soundMgr;
    contactListener.worldWidth = widthPixels / SCALE;
    world.SetContactListener(&contactListener);

    this->SCALE = widthPixels / 24.0f;

    worldWidthMeters = 24.0f;
    worldHeightMeters = heightPixels / this->SCALE;

    createWalls(widthPixels, heightPixels);
    createWinZone();
    createRacers();
}

float PhysicsWorld::randomFloat(float min, float max) {
    std::uniform_real_distribution<float> dist(min, max);
    return dist(rng);
}

void PhysicsWorld::step(float timeStep, int velIter, int posIter) {
    if (isPaused || gameOver) return;

    // --- BAJAR COOLDOWN DE LOS CUCHILLOS ---
    for (auto& k : knives) {
        if (k.cooldownTimer > 0.0f) {
            k.cooldownTimer -= timeStep;
        }
    }

    world.SetGravity(enableGravity ? b2Vec2(0.0f, 9.8f) : b2Vec2(0.0f, 0.0f));
    
    contactListener.bodiesToCheck.clear();
    //contactListener.winnerBody = nullptr;

    // 1. Dejar que Box2D calcule rebotes y resuelva colisiones
    world.Step(timeStep, velIter, posIter);

    for (size_t i = 0; i < dynamicBodies.size(); ++i) {
        if (!racerStatus[i].isAlive) continue; // Si ya murió, next.

        b2Body* racer = dynamicBodies[i];
        
        // Recorremos la lista de contactos actuales de este racer
        for (b2ContactEdge* ce = racer->GetContactList(); ce; ce = ce->next) {
            if (!ce->contact->IsTouching()) continue;

            // Obtenemos el otro cuerpo con el que choca
            b2Body* other = ce->other;

            // Chequeamos si ese "otro" es una pared nuestra
            for (const auto& wall : customWalls) {
                if (wall.body == other) {
                    // SI ES MORTAL, CHAU RACER
                    if (wall.isDeadly) {
                        racerStatus[i].isAlive = false;
                        racerStatus[i].deathPos = racer->GetPosition();
                        racer->SetEnabled(false); // Lo sacamos de la simulación
                        std::cout << ">>> RACER " << i << " MURIO EN PINCHOS <<<" << std::endl;
                        
                        // Opcional: Sonido de muerte o fx visual
                    }
                    break; // Ya encontramos la pared, salimos del loop de walls
                }
            }
        }
    }

// --- CHECK VICTORIA (CON DELAY) ---
    // 1. Detectar quién acaba de tocar la meta
    for (b2Body* winnerBody : contactListener.bodiesReachedWinZone) {
        int wIndex = -1;
        for (size_t i = 0; i < dynamicBodies.size(); ++i) {
            if (dynamicBodies[i] == winnerBody) {
                wIndex = i; break;
            }
        }

        // Si tocó la meta, está vivo, no terminó, y NO estaba ya cruzando...
        if (wIndex != -1 && !racerStatus[wIndex].hasFinished && !racerStatus[wIndex].isFinishing && racerStatus[wIndex].isAlive) {
            
            racerStatus[wIndex].isFinishing = true; // Empieza a cruzar
            racerStatus[wIndex].finishTimer = 0.0f;
            
            // Anotamos al ganador original (el primerísimo en TOCAR la meta)
            if (winnerIndex == -1) {
                winnerIndex = wIndex; 
            }
        }
    }
    contactListener.bodiesReachedWinZone.clear();

    // 2. Procesar el tiempo de delay (para los que están cruzando)
    for (size_t i = 0; i < dynamicBodies.size(); ++i) {
        if (racerStatus[i].isFinishing && !racerStatus[i].hasFinished) {
            racerStatus[i].finishTimer += timeStep;
            
            // Si ya pasó el medio segundito (o lo que configures)...
            if (racerStatus[i].finishTimer >= finishDelay) {
                racerStatus[i].hasFinished = true;
                dynamicBodies[i]->SetEnabled(false); // AHORA SÍ lo congelamos
                
                // Si el juego frena con el primero, Y este es el ganador original, cortamos todo.
                if (stopOnFirstWin && winnerIndex == (int)i) {
                    gameOver = true;
                    isPaused = true; 
                    std::cout << ">>> VICTORY: RACER " << winnerIndex << " <<<" << std::endl;
                    return;
                }
            }
        }
    }

    // 3. Si NO frena con el primero, revisamos si ya terminaron todos
    if (!stopOnFirstWin) {
        bool activeRacersLeft = false;
        for (size_t i = 0; i < dynamicBodies.size(); ++i) {
            // Sigue corriendo si está vivo y todavía no superó el delay final
            if (racerStatus[i].isAlive && !racerStatus[i].hasFinished) {
                activeRacersLeft = true;
                break;
            }
        }

        if (!activeRacersLeft) {
            gameOver = true;
            isPaused = true;
            std::cout << ">>> RACE FINISHED (No active racers left) <<<" << std::endl;
            return;
        }
    }

    // --- CHAOS MODE (Igual) ---
    if (enableChaos) {
        for (b2Body* body : contactListener.bodiesToCheck) {
            if (randomFloat(0.0f, 1.0f) < chaosChance) {
                b2Vec2 vel = body->GetLinearVelocity();
                // En modo Caos rompemos un poco la regla de 45 grados para dar variedad,
                // pero el EnforceSpeed de abajo la va a corregir al siguiente frame.
                float angleDev = randomFloat(-0.5f, 0.5f);
                float cs = cos(angleDev); float sn = sin(angleDev);
                float px = vel.x * cs - vel.y * sn;
                float py = vel.x * sn + vel.y * cs;
                body->SetLinearVelocity(b2Vec2(px * chaosBoost, py * chaosBoost));
            }
        }
    }

    // --- ENFORCE SPEED: MODO "PONG" (DIAGONALES FORZADAS) ---
    if (enforceSpeed) {
        for (b2Body* b : dynamicBodies) {
            if (!b->IsEnabled()) continue;

            b2Vec2 vel = b->GetLinearVelocity();

            // LÓGICA BINARIA:
            // No nos importa el vector. Nos importa el signo.
            // Vel X tiene que ser +Speed o -Speed.
            // Vel Y tiene que ser +Speed o -Speed.
            
            // --- EJE X ---
            if (std::abs(vel.x) < 0.1f) {
                // Si la física lo frenó (0), tiramos una moneda.
                // Esto evita el deadlock vertical.
                vel.x = (randomFloat(0.0f, 1.0f) > 0.5f) ? targetSpeed : -targetSpeed;
            } else {
                // Si ya se mueve, mantenemos el sentido pero forzamos la magnitud.
                vel.x = (vel.x > 0.0f) ? targetSpeed : -targetSpeed;
            }

            // --- EJE Y ---
            if (std::abs(vel.y) < 0.1f) {
                // Si la física lo frenó (0), tiramos una moneda.
                // Esto evita el deadlock horizontal.
                vel.y = (randomFloat(0.0f, 1.0f) > 0.5f) ? targetSpeed : -targetSpeed;
            } else {
                // Mantenemos sentido, forzamos magnitud.
                vel.y = (vel.y > 0.0f) ? targetSpeed : -targetSpeed;
            }

            // APLICAMOS LA VELOCIDAD CUADRADA
            // Nota: La velocidad total (hipotenusa) será targetSpeed * 1.414 (raiz de 2).
            // Pero cumple tu regla: "15 en X, 15 en Y".
            b->SetLinearVelocity(vel);
        }
    }

    // --- RESOLVER PICKUPS ---
// --- RESOLVER PICKUPS ---
    for (auto& ev : contactListener.pendingPickups) {
        int rIdx = getRacerIndex(ev.racer);
        int kIdx = getKnifeIndex(ev.knife);

        if (rIdx != -1 && kIdx != -1) {
            // ACÁ AGREGAMOS LA CONDICIÓN DEL COOLDOWN:
            if (!racerStatus[rIdx].hasKnife && !knives[kIdx].isPickedUp && knives[kIdx].cooldownTimer <= 0.0f) {
                racerStatus[rIdx].hasKnife = true;
                knives[kIdx].isPickedUp = true;
                knives[kIdx].ownerIndex = rIdx;
                knives[kIdx].body->SetEnabled(false); 
            }
        }
    }
    contactListener.pendingPickups.clear();

    // --- RESOLVER KILLS CON CUCHILLO ---
    for (auto& ev : contactListener.pendingKills) {
        int killerIdx = getRacerIndex(ev.killer);
        int victimIdx = getRacerIndex(ev.victim);

        if (killerIdx != -1 && victimIdx != -1) {
            // Si el asesino tiene el cuchillo y la víctima está viva
            if (racerStatus[killerIdx].hasKnife && racerStatus[victimIdx].isAlive) {
                
                // Matamos a la víctima
                racerStatus[victimIdx].isAlive = false;
                racerStatus[victimIdx].deathPos = dynamicBodies[victimIdx]->GetPosition();
                dynamicBodies[victimIdx]->SetEnabled(false);

                // El asesino suelta el cuchillo
                racerStatus[killerIdx].hasKnife = false;
                
                for (auto& k : knives) {
                    if (k.ownerIndex == killerIdx) {
                        k.isPickedUp = false;
                        k.ownerIndex = -1;
                        k.cooldownTimer = 0.2f; // <--- ACÁ LE CLAVAMOS EL SEGUNDO DE COOLDOWN
                        
                        k.body->SetTransform(dynamicBodies[killerIdx]->GetPosition(), 0);
                        k.body->SetEnabled(true);
                        break;
                    }
                }
            }
        }
    }
    contactListener.pendingKills.clear();
}

int PhysicsWorld::getWallAtPoint(float x, float y) {
    b2Vec2 point(x, y);
    for (size_t i = 0; i < customWalls.size(); ++i) {
        // Obtenemos la forma física del cuerpo
        b2Fixture* fixture = customWalls[i].body->GetFixtureList();
        // Le preguntamos a Box2D si el punto está dentro del polígono
        if (fixture && fixture->TestPoint(point)) {
            return (int)i; // Retorna el índice de la pared seleccionada
        }
    }
    return -1; // No tocamos nada
}

void PhysicsWorld::updateParticles(float dt) {
    if (isPaused) return;

    // 1. SPAWN: Procesamos los choques de este frame
    for (auto& ev : contactListener.collisionEvents) {
        // Sacamos el color de la pared
        sf::Color wallColor = sf::Color::White;
        for (const auto& w : customWalls) {
            if (w.body == ev.wall) { wallColor = w.neonColor; break; }
        }

        // Sacamos el color del racer por su índice
        sf::Color racerColor = sf::Color::White;
        for (size_t i = 0; i < dynamicBodies.size(); ++i) {
            if (dynamicBodies[i] == ev.racer) {
                if (i == 0) racerColor = sf::Color(0, 255, 255);
                else if (i == 1) racerColor = sf::Color(255, 0, 255);
                else if (i == 2) racerColor = sf::Color(57, 255, 20);
                else if (i == 3) racerColor = sf::Color(255, 215, 0);
                break;
            }
        }

        // Explotamos 4 partículas
        for(int i = 0; i < 4; i++) {
            Particle p;
            p.position = sf::Vector2f(ev.point.x * SCALE, ev.point.y * SCALE);
            p.color = (i < 2) ? wallColor : racerColor;
            
            // Le metemos una dispersión aleatoria a la normal (aprox -60 a 60 grados)
            float angleDev = randomFloat(-1.0f, 1.0f); 
            float cs = std::cos(angleDev);
            float sn = std::sin(angleDev);
            sf::Vector2f dir(
                ev.normal.x * cs - ev.normal.y * sn,
                ev.normal.x * sn + ev.normal.y * cs
            );
            
            // Velocidad inicial picante
            float speed = randomFloat(200.0f, 600.0f); 
            p.velocity = dir * speed;
            p.maxLife = 0.5f;
            p.life = p.maxLife;
            particles.push_back(p);
        }
    }
    contactListener.collisionEvents.clear();

    // 2. CINEMÁTICA: Movemos las que ya están vivas
    for (auto it = particles.begin(); it != particles.end(); ) {
        it->life -= dt;
        if (it->life <= 0.0f) {
            it = particles.erase(it); // Se murió, la borramos del vector
        } else {
            it->position += it->velocity * dt;
            it->velocity.y += 900.0f * dt; // Gravedad cruda en píxeles (caen)
            ++it;
        }
    }
}

void PhysicsWorld::spawnDebris(const CustomWall& wall) {
    b2Vec2 pos = wall.body->GetPosition();
    float angle = wall.body->GetAngle();
    float halfW = wall.width / 2.0f;
    float halfH = wall.height / 2.0f;

    // Cantidad dinámica según el área de la pared
    int numParticles = (int)(wall.width * wall.height * 5.0f);
    if (numParticles < 20) numParticles = 20;
    if (numParticles > 100) numParticles = 100;

    for (int i = 0; i < numParticles; i++) {
        Particle p;
        
        // 1. Posición aleatoria DENTRO del volumen de la pared
        float lx = randomFloat(-halfW, halfW);
        float ly = randomFloat(-halfH, halfH);
        
        // 2. Rotamos al espacio del mundo
        float wx = pos.x + (lx * std::cos(angle) - ly * std::sin(angle));
        float wy = pos.y + (lx * std::sin(angle) + ly * std::cos(angle));

        p.position = sf::Vector2f(wx * SCALE, wy * SCALE);
        p.color = wall.neonColor;
        
        // 3. Explosión violenta en 360 grados
        float vAngle = randomFloat(0.0f, 3.141592f * 2.0f);
        float speed = randomFloat(100.0f, 450.0f); 
        p.velocity = sf::Vector2f(std::cos(vAngle) * speed, std::sin(vAngle) * speed);
        
        p.maxLife = randomFloat(0.4f, 1.2f);
        p.life = p.maxLife;
        particles.push_back(p);
    }
}

// --- ACTUALIZACIÓN VISUAL Y AUDIO (SIMPLIFICADO) ---
// --- ACTUALIZACIÓN VISUAL Y AUDIO ---
void PhysicsWorld::updateWallVisuals(float dt) {
    
    // Recorremos las paredes que fueron golpeadas
    for (b2Body* body : contactListener.wallsHit) {
        
        // --- LÓGICA DE CANCIÓN ---
        int noteToPlay = -1;
        
        if (isSongLoaded && !songNotes.empty()) {
            // Sacamos la nota actual
            noteToPlay = songNotes[currentNoteIndex];
            
            // Avanzamos el indice (Loop infinito)
            currentNoteIndex = (currentNoteIndex + 1) % songNotes.size();
        }

        for (auto& wall : customWalls) {
            if (wall.body == body) {
                // 1. FLASH VISUAL
                wall.flashTimer = 1.0f;

                // Si hay canción, sobreescribimos el color del flash basado en la nota
                // Notas graves (bajas) -> Azul/Violeta. Notas agudas (altas) -> Rojo/Naranja
                if (noteToPlay != -1) {
                    // Mapeo trucho de nota MIDI (ej: 40 a 90) a índice de paleta (0 a 8)
                    int pSize = getPalette().size();
                    int colorIdx = (noteToPlay % 12) % pSize; // Usamos el semitono para el color
                    
                    // Actualizamos el color del flash dinámicamente
                    sf::Color neon = getPalette()[colorIdx];
                    wall.flashColor = sf::Color(
                        std::min(255, neon.r + 150),
                        std::min(255, neon.g + 150),
                        std::min(255, neon.b + 150)
                    );
                }

                // 2. SONIDO
                if (soundManager) {
                    if (noteToPlay != -1) {
                        // MODO CANCIÓN: Toca la nota secuencial
                        soundManager->playMidiNote(noteToPlay);
                    } else if (wall.soundID > 0) {
                        // MODO CLÁSICO: Toca el sonido de la pared
                        soundManager->playSound(wall.soundID, 0, 0);
                    }
                }

                if (wall.isDestructible && wall.currentHits > 0 && !wall.pendingDestroy) {
                wall.currentHits--;
                    if (wall.currentHits <= 0) {
                    wall.pendingDestroy = true; 
                    }
                }
            }
        }
    }

    // EJECUCIÓN DE DESTRUCCIÓN POST-CÁLCULOS
    for (int i = (int)customWalls.size() - 1; i >= 0; --i) {
        if (customWalls[i].pendingDestroy) {
            spawnDebris(customWalls[i]);
            removeCustomWall(i); // Borra el Box2D body de forma segura
        }
    }

    // Fade out
    for (auto& wall : customWalls) {
        if (wall.flashTimer > 0.0f) {
            wall.flashTimer -= dt * 3.0f;
            if (wall.flashTimer < 0.0f) wall.flashTimer = 0.0f;
        }
    }

    contactListener.wallsHit.clear();
}

// --- GUARDADO/CARGA (Sin cambios, ya soporta soundID) ---
void PhysicsWorld::saveMap(const std::string& filename) {
#ifdef __ANDROID__
    std::cerr << "Guardar mapa no esta soportado directo al APK en Android." << std::endl;
    return;
#endif
    std::ofstream file(filename);
    if (!file.is_open()) return;

    file << "CONFIG " << targetSpeed << " " << currentRacerSize << " " 
         << currentRestitution << " " << enableChaos << " " << stopOnFirstWin << "\n";
    file << "WINZONE " << winZonePos[0] << " " << winZonePos[1] << " " 
         << winZoneSize[0] << " " << winZoneSize[1] << " " << winZoneGlow << " " << "\n";

    for (const auto& w : customWalls) {
        b2Vec2 pos = w.body->GetPosition();
        // Guardamos TODO en una sola línea.
        // El orden es importante para el load.
        file << "WALL " 
             << pos.x << " " << pos.y << " " 
             << w.width << " " << w.height << " " 
             << w.soundID << " "
             << w.colorIndex << " "      
             << w.isExpandable << " "    
             << w.expansionDelay << " "
             << w.expansionSpeed << " "
             << w.expansionAxis << " "
             << w.stopOnContact << " "
             << w.stopTargetIdx << " "
             << w.maxSize << " "
             << w.shapeType << " "
             << w.rotation << " "
             << w.isDeadly << " "
             << w.isMoving << " "
             << w.pointA.x << " " << w.pointA.y << " "
             << w.pointB.x << " " << w.pointB.y << " "
             << w.moveSpeed << " "
             << w.reverseOnContact << " "
             << w.freeBounce << " "
             << w.isDestructible << " "
             << w.maxHits << " "
             << w.currentHits << " "
             << w.useTextForHP << "\n";
    }

    for (const auto& k : knives) {
        file << "KNIFE " << k.initialPos.x << " " << k.initialPos.y << "\n";
    }

    for (size_t i = 0; i < dynamicBodies.size(); ++i) {
        b2Body* b = dynamicBodies[i];
        b2Vec2 pos = b->GetPosition();
        b2Vec2 vel = b->GetLinearVelocity();
        float angle = b->GetAngle();
        float angVel = b->GetAngularVelocity();
        file << "RACER " << i << " " 
             << pos.x << " " << pos.y << " " 
             << vel.x << " " << vel.y << " " 
             << angle << " " << angVel << "\n";
    }
    file.close();
    std::cout << "Map saved: " << filename << std::endl;
}

void PhysicsWorld::loadMap(const std::string& filename) {
    sf::FileInputStream stream;
    if (!stream.open(filename)) {
        std::cerr << "Map not found: " << filename << std::endl;
        return;
    }

    // Leemos el bloque crudo desde el APK (o disco en PC) a un string
    std::string buffer;
    // SFML 3: getSize() y read() ahora devuelven std::optional
    buffer.resize(stream.getSize().value());
    stream.read(buffer.data(), buffer.size());
    
    // Lo metemos en un stream de memoria para que el resto del código no se entere del cambio
    std::istringstream file(buffer);

    clearCustomWalls();
    resetRacers(); 

    std::string line;
    while (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string type;
        ss >> type;

        if (type == "CONFIG") {
            float spd, size, rest; bool chaos;
            ss >> spd >> size >> rest >> chaos;
            targetSpeed = spd; enableChaos = chaos;
            updateRacerSize(size); updateRestitution(rest);
            
            bool stopFW;
            if (ss >> stopFW) stopOnFirstWin = stopFW;
            else stopOnFirstWin = true; // Retrocompatibilidad
        }
        else if (type == "WINZONE") {             
            float x, y, w, h;
            ss >> x >> y >> w >> h;
            updateWinZone(x, y, w, h);

            bool hasGlow = true;
            if (ss >> hasGlow) winZoneGlow = hasGlow;
            else winZoneGlow = true;
        }
        else if (type == "WALL") {
            float x, y, w, h;
            int sid = 0;
            ss >> x >> y >> w >> h >> sid;
            
            // Creamos la pared (por defecto rectangular y sin rotación)
            addCustomWall(x, y, w, h, sid, 0, 0.0f);
            CustomWall& newWall = customWalls.back();

            // 2. Color y Expansión
            int cIdx = 0;
            if (ss >> cIdx) updateWallColor(customWalls.size() - 1, cIdx);
            
            bool isExp = false;
            if (ss >> isExp) {
                newWall.isExpandable = isExp;
                ss >> newWall.expansionDelay 
                   >> newWall.expansionSpeed 
                   >> newWall.expansionAxis 
                   >> newWall.stopOnContact 
                   >> newWall.stopTargetIdx 
                   >> newWall.maxSize;
            }

            // 3. Geometría y Letalidad (NUEVO)
            int sType = 0;
            float rot = 0.0f;
            bool deadly = false;

            // Intentamos leer si existen en el archivo (retro-compatibilidad)
            if (ss >> sType) {
                newWall.shapeType = sType;
                if (ss >> rot) newWall.rotation = rot;
                if (ss >> deadly) newWall.isDeadly = deadly;

            bool isMov = false;
                if (ss >> isMov) {
                    newWall.isMoving = isMov;
                   ss >> newWall.pointA.x >> newWall.pointA.y
                       >> newWall.pointB.x >> newWall.pointB.y
                       >> newWall.moveSpeed;
                       
                    bool isRev = false;
                    if (ss >> isRev) newWall.reverseOnContact = isRev;
                    
                    bool isFree = false;
                    if (ss >> isFree) newWall.freeBounce = isFree;
                       
                    if (newWall.isMoving) {
                        newWall.body->SetType(b2_kinematicBody);
                    }
                }
                
                bool isDestr = false;
                if (ss >> isDestr) {
                    newWall.isDestructible = isDestr;
                    
                    int mHits = 3, cHits = 3;
                    if (ss >> mHits >> cHits) {
                        newWall.maxHits = mHits;
                        newWall.currentHits = cHits;
                    }
                    
                    bool useTxt = false;
                    if (ss >> useTxt) newWall.useTextForHP = useTxt;
                }

                updateCustomWall(customWalls.size() - 1, x, y, w, h, sid, sType, rot);
                customWalls.back().isDeadly = deadly;
            }
        }

        else if (type == "KNIFE") {
            float x, y;
            ss >> x >> y;
            addKnife(x, y);
        }

        else if (type == "RACER") { 
            int id; float x, y, vx, vy, a, av;
            ss >> id >> x >> y >> vx >> vy >> a >> av;
            if (id >= 0 && id < dynamicBodies.size()) {
                b2Body* b = dynamicBodies[id];
                b->SetEnabled(true);
                b->SetTransform(b2Vec2(x, y), a);
                b->SetLinearVelocity(b2Vec2(vx, vy));
                b->SetAngularVelocity(av);
                b->SetAwake(true);
            }
        }
    }
    isPaused = true;
    // file.close(); <-- Eliminado porque istringstream no tiene (ni necesita) close()
    std::cout << "Map loaded: " << filename << std::endl;
}

void PhysicsWorld::clearCustomWalls() {
    for (const auto& wall : customWalls) {
        world.DestroyBody(wall.body);
    }
    customWalls.clear();
}

sf::Color getNeonColor(int index) {
    static const std::vector<sf::Color> palette = {
        sf::Color(0, 255, 255),    // Cyan
        sf::Color(255, 0, 255),    // Magenta
        sf::Color(57, 255, 20),    // Toxic Lime
        sf::Color(255, 165, 0),    // Electric Orange
        sf::Color(147, 0, 255),    // Plasma Purple
        sf::Color(255, 0, 60),      // Hot Red
        sf::Color(255, 215, 0),    // 6: Gold (NUEVO)
        sf::Color(0, 100, 255),    // 7: Deep Blue (NUEVO)
        sf::Color(255, 105, 180)   // 8: Hot Pink (NUEVO)
    };
    return palette[index % palette.size()];
}

const std::vector<sf::Color>& PhysicsWorld::getPalette() {
    static const std::vector<sf::Color> palette = {
        sf::Color(0, 255, 255),    // 0: Cyan (Tron)
        sf::Color(255, 0, 255),    // 1: Magenta (Synthwave)
        sf::Color(57, 255, 20),    // 2: Toxic Lime
        sf::Color(255, 165, 0),    // 3: Electric Orange
        sf::Color(147, 0, 255),    // 4: Plasma Purple
        sf::Color(255, 0, 60),     // 5: Hot Red
        sf::Color(255, 215, 0),    // 6: Gold
        sf::Color(0, 100, 255),    // 7: Deep Blue
        sf::Color(255, 105, 180)   // 8: Hot Pink
    };
    return palette;
}

void PhysicsWorld::addCustomWall(float x, float y, float w, float h, int soundID, int shapeType, float rotation) {
    b2BodyDef bd; 
    bd.type = b2_staticBody; 
    bd.position.Set(x, y);
    bd.angle = rotation; // <--- Rotación Física
    
    b2Body* body = world.CreateBody(&bd);

    b2FixtureDef fd;
    fd.friction = 0.0f;
    fd.restitution = 1.0f;

    b2PolygonShape shape;
    
    if (shapeType == 1) { 
        // --- TRIÁNGULO (PINCHO) ---
        b2Vec2 vertices[3];
        // Triangulo isósceles apuntando hacia "arriba" localmente
        vertices[0].Set(0.0f, -h / 2.0f);       // Punta Superior
        vertices[1].Set(w / 2.0f, h / 2.0f);    // Base Derecha
        vertices[2].Set(-w / 2.0f, h / 2.0f);   // Base Izquierda
        shape.Set(vertices, 3);
    } else {
        // --- CAJA (NORMAL) ---
        shape.SetAsBox(w / 2.0f, h / 2.0f);
    }

    fd.shape = &shape;
    body->CreateFixture(&fd);

    CustomWall newWall;
    newWall.body = body;
    newWall.width = w;
    newWall.height = h;
    newWall.soundID = soundID;
    newWall.shapeType = shapeType; 
    newWall.rotation = rotation;

    // Si es pincho, es mortal y rojo por defecto
    if (shapeType == 1) {
        newWall.isDeadly = true;
        if (soundID == 0) newWall.colorIndex = 5; // 5 = Hot Red
    }

    // Lógica de colores (igual que antes)
    int colorIdx = (soundID > 0) ? (soundID - 1) : newWall.colorIndex;
    if (shapeType == 1 && soundID == 0) colorIdx = 5; // Force Red

    // Forzamos rango
    if (colorIdx < 0) colorIdx = 0;
    colorIdx = colorIdx % getPalette().size();

    newWall.colorIndex = colorIdx;
    
    const auto& pal = getPalette();
    sf::Color neon = pal[colorIdx];
    newWall.baseFillColor = sf::Color(neon.r / 5, neon.g / 5, neon.b / 5, 240);
    newWall.neonColor = neon;
    newWall.flashColor = sf::Color(
        std::min(255, neon.r + 100),
        std::min(255, neon.g + 100),
        std::min(255, neon.b + 100)
    );

    customWalls.push_back(newWall);
}

int PhysicsWorld::getRacerIndex(b2Body* body) const {
    for (size_t i = 0; i < dynamicBodies.size(); ++i) {
        if (dynamicBodies[i] == body) return (int)i;
    }
    return -1;
}

int PhysicsWorld::getKnifeIndex(b2Body* body) const {
    for (size_t i = 0; i < knives.size(); ++i) {
        if (knives[i].body == body) return (int)i;
    }
    return -1;
}

void PhysicsWorld::addKnife(float x, float y) {
    b2BodyDef bd;
    bd.type = b2_staticBody; // Estático para que no ruede ni tenga física real de peso
    bd.position.Set(x, y);
    
    b2Body* body = world.CreateBody(&bd);
    
    b2PolygonShape shape;
    shape.SetAsBox(0.5f, 0.5f); // Hitbox del item
    
    b2FixtureDef fd;
    fd.shape = &shape;
    fd.isSensor = true; // Lo hace atravesable
    body->CreateFixture(&fd);
    
    KnifeItem knife;
    knife.body = body;
    knife.initialPos = b2Vec2(x, y); // Guardamos dónde nació
    knives.push_back(knife);
}

void PhysicsWorld::removeKnife(int index) {
    if (index < 0 || index >= knives.size()) return;
    world.DestroyBody(knives[index].body);
    knives.erase(knives.begin() + index);
}

void PhysicsWorld::updateKnifePos(int index, float x, float y) {
    if (index < 0 || index >= knives.size()) return;
    knives[index].initialPos.Set(x, y);
    knives[index].body->SetTransform(b2Vec2(x, y), 0);
}

void PhysicsWorld::clearKnives() {
    for (auto& k : knives) {
        if (k.body) world.DestroyBody(k.body);
    }
    knives.clear();
}

void PhysicsWorld::updateWallColor(int index, int newColorIndex) {
    if (index < 0 || index >= customWalls.size()) return;
    
    CustomWall& w = customWalls[index];
    const auto& pal = getPalette();
    
    // Safety check
    if (newColorIndex < 0) newColorIndex = 0;
    newColorIndex = newColorIndex % pal.size();

    w.colorIndex = newColorIndex;
    sf::Color neon = pal[newColorIndex];
    
    w.baseFillColor = sf::Color(neon.r / 5, neon.g / 5, neon.b / 5, 240);
    w.neonColor = neon;
    w.flashColor = sf::Color(
        std::min(255, neon.r + 100),
        std::min(255, neon.g + 100),
        std::min(255, neon.b + 100)
    );
}

void PhysicsWorld::updateCustomWall(int index, float x, float y, float w, float h, int soundID, int shapeType, float rotation) {
    if (index < 0 || index >= customWalls.size()) return;
    CustomWall& wall = customWalls[index];
    
    bool needRebuild = (wall.width != w || wall.height != h || wall.shapeType != shapeType);
    
    wall.soundID = soundID;
    wall.shapeType = shapeType;
    wall.rotation = rotation;
    wall.width = w;
    wall.height = h;

    // Si cambia a pincho, lo hacemos mortal y rojo
    if (shapeType == 1) {
        wall.isDeadly = true;
        updateWallColor(index, 5);
    } else {
        // Si vuelve a ser pared, le sacamos lo mortal (opcional, capaz querés pared mortal)
        // wall.isDeadly = false; 
    }

    wall.body->SetTransform(b2Vec2(x, y), rotation);

    if (needRebuild) {
        wall.body->DestroyFixture(wall.body->GetFixtureList());
        
        b2FixtureDef fd;
        fd.friction = 0.0f;
        fd.restitution = 1.0f;
        b2PolygonShape shape;

        if (shapeType == 1) { // Triángulo
            b2Vec2 vertices[3];
            vertices[0].Set(0.0f, -h / 2.0f);
            vertices[1].Set(w / 2.0f, h / 2.0f);
            vertices[2].Set(-w / 2.0f, h / 2.0f);
            shape.Set(vertices, 3);
        } else { // Caja
            shape.SetAsBox(w / 2.0f, h / 2.0f);
        }

        fd.shape = &shape;
        wall.body->CreateFixture(&fd);
    }
}

void PhysicsWorld::removeCustomWall(int index) {
    if (index < 0 || index >= customWalls.size()) return;
    if (customWalls[index].borderSide != -1) return; // No borres muros de borde
    world.DestroyBody(customWalls[index].body);
    customWalls.erase(customWalls.begin() + index);
}

void PhysicsWorld::updateWallExpansion(float dt) {
    if (isPaused) return;

    for (size_t i = 0; i < customWalls.size(); ++i) {
        CustomWall& wall = customWalls[i];

        if (!wall.isExpandable) continue;

        wall.timeAlive += dt;
        if (wall.timeAlive < wall.expansionDelay) continue;

        float growth = wall.expansionSpeed * dt;
        float newWidth = wall.width;
        float newHeight = wall.height;
        bool sizeChanged = false;

        // Calcular crecimiento potencial
        if (wall.expansionAxis == 0 || wall.expansionAxis == 2) { newWidth += growth; sizeChanged = true; }
        if (wall.expansionAxis == 1 || wall.expansionAxis == 2) { newHeight += growth; sizeChanged = true; }

        // Si no se supone que crezca, pasamos al siguiente (pero ojo, si ya creció antes, igual mata)
        if (!sizeChanged) continue;

        // --- CHECK 1: MAX SIZE ---
        if (wall.maxSize > 0.0f) {
            float checkDim = (wall.expansionAxis == 0) ? newWidth : newHeight;
            if (wall.expansionAxis == 2) checkDim = std::max(newWidth, newHeight);

            if (checkDim >= wall.maxSize) {
                wall.isExpandable = false;
                if (wall.expansionAxis == 0 || wall.expansionAxis == 2) newWidth = wall.maxSize;
                if (wall.expansionAxis == 1 || wall.expansionAxis == 2) newHeight = wall.maxSize;
            }
        }

        // --- CHECK 2: STOP ON SPECIFIC CONTACT ---
        if (wall.stopOnContact) {
            b2Vec2 myPos = wall.body->GetPosition();
            
            for (size_t j = 0; j < customWalls.size(); ++j) {
                if (i == j) continue; 
                if (wall.stopTargetIdx != -1 && (int)j != wall.stopTargetIdx) continue;

                const CustomWall& other = customWalls[j];
                b2Vec2 otherPos = other.body->GetPosition();

                float dx = std::abs(myPos.x - otherPos.x);
                float dy = std::abs(myPos.y - otherPos.y);

                float sumHalfWidths = (newWidth / 2.0f) + (other.width / 2.0f);
                float sumHalfHeights = (newHeight / 2.0f) + (other.height / 2.0f);

                // Pequeño margen de 0.05 para que frene JUSTO antes de tocar
                if (dx < sumHalfWidths - 0.05f && dy < sumHalfHeights - 0.05f) {
                    wall.isExpandable = false; 
                    // Ajustamos el tamaño para que sea "contacto perfecto"
                    // (Esto es opcional, pero evita que queden gaps feos)
                    // Por ahora simplemente frenamos el crecimiento.
                    newWidth = wall.width; 
                    newHeight = wall.height;
                    sizeChanged = false; 
                    std::cout << "Wall " << i << " stopped by target Wall " << j << std::endl;
                    break; 
                }
            }
        }

// >>> ZONA DE CRUSH 2.0 (CORREGIDA) <<<
        b2Vec2 wallPos = wall.body->GetPosition();
        float wallHalfW = (sizeChanged ? newWidth : wall.width) / 2.0f;
        float wallHalfH = (sizeChanged ? newHeight : wall.height) / 2.0f;
        
        float racerRadius = currentRacerSize / 2.0f; 
        
        // ACÁ PODÉS JUGAR CON EL PORCENTAJE (0.9 = 90% aplastado para morir)
        float killPercentage = 0.5f; 
        float killThresholdArea = (currentRacerSize * currentRacerSize) * killPercentage;

        for (size_t r = 0; r < dynamicBodies.size(); ++r) {
            if (!racerStatus[r].isAlive) continue;

            b2Body* racerBody = dynamicBodies[r];
            if (!racerBody->IsEnabled()) continue;

            b2Vec2 racerPos = racerBody->GetPosition();

            float dx = std::abs(racerPos.x - wallPos.x);
            float dy = std::abs(racerPos.y - wallPos.y);

            float rawOverlapX = (wallHalfW + racerRadius) - dx;
            float rawOverlapY = (wallHalfH + racerRadius) - dy;

            if (rawOverlapX > 0.05f && rawOverlapY > 0.05f) {
                // --- EL FIX MATEMÁTICO ---
                // El overlap no puede ser mayor que el propio racer.
                // Si la pared es gigante, 'rawOverlap' es gigante. Lo recortamos al tamaño del racer.
                float realOverlapX = std::min(rawOverlapX, currentRacerSize);
                float realOverlapY = std::min(rawOverlapY, currentRacerSize);

                float crushedArea = realOverlapX * realOverlapY;
                
                if (crushedArea > killThresholdArea) {
                    std::cout << ">>> RACER " << r << " SQUASHED (" << (crushedArea/(currentRacerSize*currentRacerSize))*100 << "%) <<<" << std::endl;
                    
                    racerStatus[r].isAlive = false;
                    racerStatus[r].deathPos = racerPos;
                    racerBody->SetEnabled(false); 
                }
            }
        }
        // >>> FIN ZONA DE CRUSH <<<

        // Solo actualizamos Box2D si realmente creció
        if (!sizeChanged || (newWidth == wall.width && newHeight == wall.height)) continue;

        // Actualizar física de la pared
        wall.width = newWidth;
        wall.height = newHeight;
        wall.body->DestroyFixture(wall.body->GetFixtureList());
        b2PolygonShape box;
        box.SetAsBox(wall.width / 2.0f, wall.height / 2.0f);
        b2FixtureDef fd;
        fd.shape = &box;
        fd.friction = 0.0f;
        fd.restitution = 1.0f;
        wall.body->CreateFixture(&fd);
    }
}

void PhysicsWorld::updateMovingPlatforms(float dt) {
    if (isPaused) return;

    for (size_t i = 0; i < customWalls.size(); ++i) {
        CustomWall& wall = customWalls[i];
        if (!wall.isMoving) continue;

        if (wall.body->GetType() != b2_kinematicBody) {
            wall.body->SetType(b2_kinematicBody);
        }

        b2Vec2 currentPos = wall.body->GetPosition();
        b2Vec2 vel = wall.body->GetLinearVelocity();

        // Si está quieta (ej. recién creada), le damos el empujón inicial
        if (vel.LengthSquared() < 0.01f && !wall.isFreeBouncing) {
            b2Vec2 targetPos = wall.movingTowardsB ? wall.pointB : wall.pointA;
            b2Vec2 dir = targetPos - currentPos;
            if (dir.LengthSquared() > 0.0f) {
                dir.Normalize();
                vel = wall.moveSpeed * dir; // FIX: float * vector
            }
        }

        bool hitTarget = false;
        // Solo chequeamos si llegó a A o B si NO está en modo rebote libre
        if (!wall.isFreeBouncing) {
            b2Vec2 targetPos = wall.movingTowardsB ? wall.pointB : wall.pointA;
            b2Vec2 dir = targetPos - currentPos;
            if (dir.Length() < 0.1f) hitTarget = true;
        }

        bool hitWall = false;

        // CHECK PREDICTIVO (AABB)
        if (wall.reverseOnContact && !hitTarget) {
            b2Vec2 nextPos = currentPos + (dt * vel); // FIX: float * vector
            float myHalfW = wall.width / 2.0f;
            float myHalfH = wall.height / 2.0f;
            
            for (size_t j = 0; j < customWalls.size(); ++j) {
                if (i == j) continue; 
                
                const CustomWall& other = customWalls[j];
                b2Vec2 otherPos = other.body->GetPosition();
                float otherHalfW = other.width / 2.0f;
                float otherHalfH = other.height / 2.0f;
                
                float dx = std::abs(nextPos.x - otherPos.x);
                float dy = std::abs(nextPos.y - otherPos.y);
                
                if (dx < (myHalfW + otherHalfW - 0.02f) && dy < (myHalfH + otherHalfH - 0.02f)) {
                    hitWall = true;
                    break;
                }
            }
        }

        if (hitTarget) {
            // Llegó a A o B de forma normal
            wall.movingTowardsB = !wall.movingTowardsB;
            b2Vec2 newTarget = wall.movingTowardsB ? wall.pointB : wall.pointA;
            b2Vec2 newDir = newTarget - currentPos;
            if (newDir.LengthSquared() > 0.0f) {
                newDir.Normalize();
                vel = wall.moveSpeed * newDir; 
            }
        } else if (hitWall) {
            // CHOCÓ CON OTRA PARED: Literalmente invertimos el vector de velocidad
            vel = -1.0f * vel; 
            
            if (wall.freeBounce) {
                // Si la opción está prendida, cortamos amarras con A y B
                wall.isFreeBouncing = true; 
            } else {
                // Si no, formalmente cambia de objetivo (para que no se tranque)
                wall.movingTowardsB = !wall.movingTowardsB;
            }
        }

        wall.body->SetLinearVelocity(vel);
    }
}

std::vector<CustomWall>& PhysicsWorld::getCustomWalls() { return customWalls; }
b2Body* PhysicsWorld::getWinZoneBody() const { return winZoneBody; }
void PhysicsWorld::createWinZone() { b2BodyDef bd; bd.type=b2_staticBody; winZonePos[0]=worldWidthMeters/1.0f; winZonePos[1]=worldHeightMeters*0.8f; bd.position.Set(winZonePos[0], winZonePos[1]); winZoneBody=world.CreateBody(&bd); b2PolygonShape b; b.SetAsBox(winZoneSize[0]/2, winZoneSize[1]/2); b2FixtureDef fd; fd.shape=&b; fd.isSensor=true; winZoneBody->CreateFixture(&fd); contactListener.winZoneBody=winZoneBody; }
void PhysicsWorld::updateWinZone(float x, float y, float w, float h) { if(!winZoneBody)return; winZoneBody->SetTransform(b2Vec2(x,y),0); winZoneBody->DestroyFixture(winZoneBody->GetFixtureList()); b2PolygonShape b; b.SetAsBox(w/2,h/2); b2FixtureDef fd; fd.shape=&b; fd.isSensor=true; winZoneBody->CreateFixture(&fd); winZonePos[0]=x;winZonePos[1]=y;winZoneSize[0]=w;winZoneSize[1]=h; }
void PhysicsWorld::updateRacerSize(float newSize) { currentRacerSize=newSize; for(b2Body* b:dynamicBodies){ b->DestroyFixture(b->GetFixtureList()); b2PolygonShape s; s.SetAsBox(newSize/2,newSize/2); b2FixtureDef fd; fd.shape=&s; fd.density=1; fd.friction=currentFriction; fd.restitution=currentRestitution; b->CreateFixture(&fd); b->SetAwake(true); } }
void PhysicsWorld::updateRestitution(float newRest) { currentRestitution=newRest; for(auto b:dynamicBodies) for(auto f=b->GetFixtureList();f;f=f->GetNext()) f->SetRestitution(newRest); }
void PhysicsWorld::updateFriction(float newFriction) { currentFriction=newFriction; for(auto b:dynamicBodies) for(auto f=b->GetFixtureList();f;f=f->GetNext()) f->SetFriction(newFriction); }
void PhysicsWorld::updateFixedRotation(bool fixed) { currentFixedRotation=fixed; for(auto b:dynamicBodies) { b->SetFixedRotation(fixed); b->SetAwake(true); } }
const std::vector<b2Body*>& PhysicsWorld::getDynamicBodies() const { return dynamicBodies; }
void PhysicsWorld::resetRacers() { 
    // --- REVIVIR A TODOS ---
for(auto& status : racerStatus) {
        status.isAlive = true;
        status.hasFinished = false;
        status.isFinishing = false; // <---
        status.finishTimer = 0.0f;  // <---
        status.hasKnife = false;     // <---
    }

    for(auto& k : knives) {
        k.isPickedUp = false;
        k.ownerIndex = -1;
        k.body->SetTransform(k.initialPos, 0); // Vuelve al spawn original
        k.body->SetEnabled(true);
    }

    int i = 0; 
    for(auto b : dynamicBodies){ 
        float x = (worldWidthMeters/5.0f)*(i+1); 
        float y = worldHeightMeters/2.0f; 
        
        b->SetEnabled(true); // <--- CORREGIDO: Usamos SetEnabled en lugar de SetActive
        
        b->SetTransform(b2Vec2(x, y), 0); 
        b->SetLinearVelocity(b2Vec2(targetSpeed, targetSpeed)); 
        b->SetAngularVelocity(0); 
        b->SetAwake(true); 
        i++; 
    } 
    gameOver = false; 
    winnerIndex = -1; 
    isPaused = true; 
}

void PhysicsWorld::duplicateCustomWall(int index) {
    if (index < 0 || index >= customWalls.size()) return;

    const CustomWall& original = customWalls[index];
    b2Vec2 pos = original.body->GetPosition();

    // Creamos la pared base desfasada 1 metro en X e Y para que se note en pantalla
    addCustomWall(pos.x + 1.0f, pos.y + 1.0f, original.width, original.height, original.soundID, original.shapeType, original.rotation);

    CustomWall& newWall = customWalls.back();

    // --- COPIAMOS TODAS LAS PROPIEDADES A MANO ---
    newWall.isExpandable = original.isExpandable;
    newWall.expansionDelay = original.expansionDelay;
    newWall.expansionSpeed = original.expansionSpeed;
    newWall.expansionAxis = original.expansionAxis;
    newWall.stopOnContact = original.stopOnContact;
    newWall.stopTargetIdx = original.stopTargetIdx;
    newWall.maxSize = original.maxSize;
    newWall.isDeadly = original.isDeadly;

    newWall.isMoving = original.isMoving;
    // Si se mueve, le desfasamos la ruta también para que corra en paralelo
    newWall.pointA = original.pointA + b2Vec2(1.0f, 1.0f);
    newWall.pointB = original.pointB + b2Vec2(1.0f, 1.0f);
    newWall.moveSpeed = original.moveSpeed;
    newWall.movingTowardsB = original.movingTowardsB;
    newWall.reverseOnContact = original.reverseOnContact;
    newWall.freeBounce = original.freeBounce;
    newWall.isFreeBouncing = original.isFreeBouncing;

    // Colores y neones
    newWall.colorIndex = original.colorIndex;
    newWall.baseFillColor = original.baseFillColor;
    newWall.neonColor = original.neonColor;
    newWall.flashColor = original.flashColor;

    // Actualizamos el tipo de cuerpo en Box2D si es una plataforma móvil
    if (newWall.isMoving) {
        newWall.body->SetType(b2_kinematicBody);
    }
}

void PhysicsWorld::createWalls(float widthPixels, float heightPixels) {
    mapWidth = worldWidthMeters;
    mapHeight = worldHeightMeters;
    float thick = 0.5f;

    // AHORA LAS PAREDES DEL BORDE TIENEN SONIDO Y COLOR
    addCustomWall(mapWidth / 2.0f, mapHeight, mapWidth, thick, 1); // Piso
    customWalls.back().borderSide = 0;
    
    addCustomWall(mapWidth / 2.0f, 0.0f, mapWidth, thick, 1);   // Techo
    customWalls.back().borderSide = 1;
    
    addCustomWall(0.0f, mapHeight / 2.0f, thick, mapHeight, 1); // Izq
    customWalls.back().borderSide = 2;
    
    addCustomWall(mapWidth, mapHeight / 2.0f, thick, mapHeight, 1);// Der
    customWalls.back().borderSide = 3;
}

void PhysicsWorld::updateMapBounds(float newW, float newH) {
    mapWidth = newW;
    mapHeight = newH;
    float thick = 0.5f;
    
    for (size_t i = 0; i < customWalls.size(); i++) {
        CustomWall& w = customWalls[i];
        // Si es un borde, le recalculamos tamaño y posición en caliente usando tu propia función
        if (w.borderSide == 0) updateCustomWall(i, mapWidth / 2.0f, mapHeight, mapWidth, thick, w.soundID, w.shapeType, w.rotation);
        else if (w.borderSide == 1) updateCustomWall(i, mapWidth / 2.0f, 0.0f, mapWidth, thick, w.soundID, w.shapeType, w.rotation);
        else if (w.borderSide == 2) updateCustomWall(i, 0.0f, mapHeight / 2.0f, thick, mapHeight, w.soundID, w.shapeType, w.rotation);
        else if (w.borderSide == 3) updateCustomWall(i, mapWidth, mapHeight / 2.0f, thick, mapHeight, w.soundID, w.shapeType, w.rotation);
    }
}


void PhysicsWorld::createRacers() { 
    float s = currentRacerSize; 
    b2PolygonShape b; 
    b.SetAsBox(s/2, s/2); 
    b2FixtureDef fd; 
    fd.shape = &b; 
    fd.density = 1; 
    fd.friction = currentFriction; 
    fd.restitution = currentRestitution; 
    
    for(int i=0; i<4; ++i){ 
        b2BodyDef bd; 
        bd.type = b2_dynamicBody; 
        bd.bullet = true; 
        bd.fixedRotation = currentFixedRotation; 
        bd.position.Set((worldWidthMeters/5.0f)*(i+1), worldHeightMeters/2.0f); 
        b2Body* bod = world.CreateBody(&bd); 
        bod->CreateFixture(&fd); 
        bod->SetLinearVelocity(b2Vec2(targetSpeed, targetSpeed)); 
        dynamicBodies.push_back(bod); 
    }
    
    // --- INICIALIZAR ESTADO DE VIDA ---
    racerStatus.clear();
    racerStatus.resize(dynamicBodies.size(), {true, false, false, 0.0f, {0,0}});
}

void PhysicsWorld::loadSong(const std::string& filename) {
    sf::FileInputStream stream;
    if (!stream.open(filename)) {
        std::cerr << "Error cargando cancion: " << filename << std::endl;
        return;
    }

    std::string buffer;
    // SFML 3: getSize() y read() ahora devuelven std::optional
    buffer.resize(stream.getSize().value());
    stream.read(buffer.data(), buffer.size());
    std::istringstream file(buffer);

    songNotes.clear();
    currentNoteIndex = 0;
    std::string line;
    bool readingNotes = false;

    while (std::getline(file, line)) {
        if (line == "SONG_START") {
            readingNotes = true;
            continue;
        }
        if (line == "SONG_END") {
            break;
        }
        if (readingNotes) {
            try {
                int note = std::stoi(line);
                songNotes.push_back(note);
            } catch (...) {}
        }
    }
    
    isSongLoaded = !songNotes.empty();
    std::cout << "Cancion cargada! Notas: " << songNotes.size() << std::endl;
}