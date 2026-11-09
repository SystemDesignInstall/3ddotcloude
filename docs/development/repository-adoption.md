# Реестр репозиториев Spatial Platform и первый этап совмещения фото с A2

Срез на 2026-11-09. Область: Spatial Platform / 3D photogrametry. Реестр объединяет репозитории, найденные в доступной истории проекта, архитектурной матрице SP-ARCH-MATRIX-0001, отчётах по A2 и архитектурным пробелам, исходниках и реестрах зависимостей. MVR VIEW, LEDMAP и OpenFollow относятся к отдельным проектам. Запись в реестре фиксирует назначение технологии и степень принятия. Проверка лицензии привязана к конкретной версии и составу сборки.

Состояние основного кода сверено с `main` на `afa34be`. Импорт A2 и совместный просмотр облака с маршрутом находятся в открытом [PR #18](https://github.com/SystemDesignInstall/3ddotcloude/pull/18) на `755ecbe`. [Сборка Windows с A2](https://github.com/SystemDesignInstall/3ddotcloude/actions/runs/42517736802) прошла проверки. Реальный парный набор фото + A2 для проверки совмещения ещё нужен.

## Что уже есть

| Репозиторий | Состояние и решение |
|---|---|
| [3ddotcloude](https://github.com/SystemDesignInstall/3ddotcloude) | Основной код, C++ ядро и отдельное исследовательское приложение Spatial Photo Cloud. Точка входа для первого прототипа — `python/research/photo_to_cloud`. |
| [spatial-rfcs](https://github.com/SystemDesignInstall/spatial-rfcs) | Конституция, ADR и RFC. Изменение канонических контрактов идёт через этот репозиторий. |
| [COLMAP / PyCOLMAP](https://github.com/colmap/colmap) | Основной SfM, позы, калибровка камер и разреженная геометрия. C++ адаптер и Python путь существуют отдельно. В Photo Cloud PyCOLMAP также строит Poisson-сетку из PLY с нормалями. Сохраняем текущий фото-процесс. |
| [OpenMVS](https://github.com/cdcseacave/openMVS) | Внешний процесс в Photo Cloud для плотного облака и текстурирования. Уже используется как отдельный движок. Для поставки отдельно проверять AGPL и состав дистрибутива. |
| [COLMAP-CL](https://github.com/openphotogrammetry/colmap-cl) | Внешний экспериментальный путь для AMD OpenCL. В коде есть проверка устройства и откат к CPU. Включение зависит от результата проверки на рабочей машине. |
| [GTSAM](https://github.com/borglab/gtsam) | Адаптер C++ и стадии оптимизации траектории и BA существуют. Переиспользуем для подтверждённых ограничений графа поз при развитии Core. |
| [laspy](https://github.com/laspy/laspy) | Импорт LAS из A2 в PR #18, сохранение исходного LAS и производный PLY. Путь LAZ требует отдельной реализации. |
| [rawpy](https://github.com/letmaik/rawpy), [LibRaw](https://github.com/LibRaw/LibRaw), [Pillow](https://github.com/python-pillow/Pillow), [ExifRead](https://github.com/ianare/exif-py), [Piexif](https://github.com/hMatoba/Piexif) | Уже обслуживают фото-ввод и подготовку рабочих RGB-копий CR2 с метаданными. Оригиналы остаются источником. |
| [NumPy](https://github.com/numpy/numpy), [PyOpenCL](https://github.com/inducer/pyopencl), [PyInstaller](https://github.com/pyinstaller/pyinstaller) | Уже применяются для численных операций, диагностики OpenCL и упаковки Windows. Для начального расчёта совмещения достаточно NumPy. |
| [Eigen](https://gitlab.com/libeigen/eigen), [Protobuf](https://github.com/protocolbuffers/protobuf), [SQLite](https://www.sqlite.org/), [nlohmann/json](https://github.com/nlohmann/json), [GoogleTest](https://github.com/google/googletest), [Conan Center Index](https://github.com/conan-io/conan-center-index) | Инфраструктура C++, хранение, протокол и тесты. Рецепт GTSAM и версии проверяются по `conan.lock` и `THIRD_PARTY.yml`. |

## Репозитории Werdna0107

| Репозиторий | Что берём и когда |
|---|---|
| [A2-slam](https://github.com/Werdna0107/A2-slam) | Форматы LAS/TXT/CSV, папку съёмки, отчёт и диагностический просмотр траектории. Импорт уже в PR #18. Закрытый движок остаётся внешним источником результатов. |
| [OpenSLAM](https://github.com/Werdna0107/OpenSLAM) | Практический образец комплектации скана Livox и полевого контроля. Для тестового набора фиксируем устройство, прошивку, формат записи и версию экспорта. |
| [mSLAM](https://github.com/Werdna0107/mSLAM) | Исторический референс офлайн обработки записи LiDAR на телефоне и просмотра LAS. Старый URL вернул 404 через GitHub API при текущей проверке, хотя сохранённая поисковая копия доступна. |
| [atlas](https://github.com/Werdna0107/atlas) | Управление съёмкой Insta360, интервалы и сохранение исходных кадров. Кандидат для ветки 360° захвата. |
| [Slam-player](https://github.com/Werdna0107/Slam-player) | Воспроизведение маршрута и постепенная отрисовка облака. Диагностику по времени берём после подтверждения временной привязки точек. |
| [A2-PointCloud-Draft](https://github.com/Werdna0107/A2-PointCloud-Draft) | Сечения XY/XZ/YZ, выбор точек на облаке, обводка и экспорт DXF/PNG. Отдельная задача просмотрщика после совмещения. |
| [A2-colorizer](https://github.com/Werdna0107/A2-colorizer) | Сценарий LAS + траектория + 360° видео → цвет и E57. Берём интерфейс калибровки и диагностику окраски. Для нашей реализации нужны позы камеры, модель камеры, временная связь для видео и проверка видимости точек. |

## Геометрия, SLAM и оценка качества

| Репозиторий | Роль в проекте |
|---|---|
| [Open3D](https://github.com/isl-org/Open3D) | Первый кандидат на уточнение совмещения через point-to-plane ICP после начального преобразования. Проверяем выборку, нормали, перекрытие и остатки. Отдельная опциональная зависимость после проверки на Windows. |
| [TEASER++](https://github.com/MIT-SPARK/TEASER-plusplus) | Кандидат на устойчивое начальное совмещение, когда уже имеются шумные 3D-соответствия. Сравниваем после получения базовой линии по ручным точкам. |
| [evo](https://github.com/MichaelGrupp/evo) | Внешний эталон отчётов APE/RPE при наличии сопоставимой эталонной траектории. Явно фиксируем ассоциацию времени, кадр и режим выравнивания. |
| [KISS-ICP](https://github.com/PRBonn/kiss-icp) | Базовая линия одометрии для исходных LiDAR-кадров. Нужна запись с последовательностью кадров. |
| [LIO-SAM](https://github.com/TixiaoShan/LIO-SAM) | Референс LiDAR/IMU, GPS-факторов и инкрементального графа. Проверять на синхронизированных сырых данных. |
| [FAST-LIVO2](https://github.com/hku-mars/FAST-LIVO2) | Кандидат для синхронного LiDAR + IMU + камера, включая референс калибровки. Это отдельный сценарий от совмещения независимо снятых фото и LAS. |
| [GLIM](https://github.com/koide3/glim) | Референс подкарты и глобальной оптимизации LiDAR. Сравнивать на исходных кадрах. |
| [hdl_graph_slam](https://github.com/koide3/hdl_graph_slam) | Исторический референс факторов плоскости пола, GPS и межкадровых ограничений. |
| [RTAB-Map](https://github.com/introlab/rtabmap) | Сохранять доказательства принятия/отбраковки связей и результаты замыкания петель. Кандидат для сравнения на RGB-D. |
| [maplab](https://github.com/ethz-asl/maplab) | Отдельные сессии и их системы координат, регистрация и объединение только после подтверждения связи. Архитектурный референс. |
| [ORB-SLAM3](https://github.com/UZ-SLAMLab/ORB_SLAM3) | Референс нескольких карт, visual/VIO и коррекции масштаба Sim(3). Отдельный исследовательский прогон при задаче визуальной одометрии. |
| [OpenVINS](https://github.com/rpng/open_vins) | Референс визуально-инерциальной одометрии, соглашений кватернионов и калибровки IMU. |
| [g2o](https://github.com/RainerKuemmerle/g2o) | Референс робастных ядер, информационных матриц и ошибок рёбер. Основной решатель проекта — уже имеющийся GTSAM. |
| [Ceres Solver](https://github.com/ceres-solver/ceres-solver) | Референс и возможный отдельный решатель для BA или устойчивой геометрической оптимизации. Первое совмещение выполняется закрытым численным решением на NumPy. |

## Альтернативные фото-процессы, калибровка и обмен

| Репозиторий | Что берём |
|---|---|
| [openMVG](https://github.com/openMVG/openMVG), [AliceVision](https://github.com/alicevision/AliceVision) | Раздельные представления кадра, калибровки, позы и рига. Сравнение SfM при воспроизводимом сбое текущего backend. |
| [GLOMAP](https://github.com/colmap/glomap) | Глобальный SfM для сравнения с инкрементальным методом. Upstream перенёс реализацию в COLMAP, поэтому отдельный движок не требуется. |
| [OpenSfM](https://github.com/mapillary/OpenSfM) | Сферическая камера и топоцентрический кадр. Кандидат для сравнения с перспективными проекциями из Insta360. |
| [MicMac](https://github.com/micmacIGN/micmac) | Инженерный workflow уравнивания и контрольных точек. Референс независимой проверки. |
| [TeleSculptor](https://github.com/Kitware/TeleSculptor) | Модульный сценарий воздушных фото и видео с метаданными. Референс при расширении набора источников. |
| [CARS](https://github.com/CNES/cars) | Разбиение плотной стереообработки на блоки. Референс для больших проектов. |
| [ODM](https://github.com/OpenDroneMap/ODM), [NodeODM](https://github.com/OpenDroneMap/NodeODM) | Поэтапные расчёты, частичный повтор запуска, состояние задач и отчёты. Референс планировщика, отдельная ветка аэросъёмки. |
| [CamOdoCal](https://github.com/hengli/camodocal), [Kalibr](https://github.com/ethz-asl/kalibr) | Раздельные intrinsics, extrinsics и временное смещение. Референсы для калибровки рига до привязки камеры к сканеру. |
| [OpenCV](https://github.com/opencv/opencv), [FFmpeg](https://github.com/FFmpeg/FFmpeg) | Компьютерное зрение и извлечение кадров с временными метками для ветки видео/360°. Оригинал и происхождение производных кадров сохраняются. |
| [PDAL](https://github.com/PDAL/PDAL), [LASzip](https://github.com/LASzip/LASzip), [libE57Format](https://github.com/asmaloney/libE57Format) | Потоковые преобразования облаков, LAZ и E57. Приоритет появляется с реальным обменным набором и проверкой сохранения полей. |
| [PROJ](https://github.com/OSGeo/PROJ), [GDAL](https://github.com/OSGeo/gdal), [GeoLibre](https://github.com/opengeos/GeoLibre) | Явные CRS и операции преобразования, GIS-обмен и картографическое представление. Регистрация локальных облаков и преобразование CRS учитываются отдельно. |
| [SuperPoint](https://github.com/magicleap/SuperPointPretrainedNetwork) | Ранее обсуждавшийся исследовательский детектор признаков. Рассматривать только для проблемных сцен при проверке условий кода и весов. |
| [DBoW2](https://github.com/dorian3d/DBoW2), [NetVLAD](https://github.com/Relja/netvlad), [SuperGlue](https://github.com/magicleap/SuperGluePretrainedNetwork) | Референсы поиска кандидатов на замыкание и сопоставления признаков из проекта P3. Совпадение признаков проходит геометрическую проверку перед принятием ограничения. |

## AI и визуальные представления

| Репозиторий | Решение |
|---|---|
| [LingBot-Map](https://github.com/Robbyant/lingbot-map) | Публичный upstream подтверждён по адресу Robbyant. Запись об ошибке 404 в старой матрице относится к другому адресу. Отдельный исследовательский benchmark потоковой реконструкции и сверка кода, весов, frame/scale и Windows/AMD. |
| [LingBot-Vision](https://github.com/Robbyant/lingbot-vision), [LingBot-Depth](https://github.com/Robbyant/lingbot-depth) | Исследовательские признаки и уточнение глубины. Сохранять как отдельные наблюдения/гипотезы после валидации. |
| [LingBot-Video](https://github.com/Robbyant/lingbot-video), [LingBot-VA](https://github.com/Robbyant/lingbot-va) | Референсы видео и моделей действий. За пределами первого контура реконструкции. |
| [LingBot-World](https://github.com/Robbyant/lingbot-world), [LingBot-World v2](https://github.com/Robbyant/lingbot-world-v2) | Исследовательские генеративные миры. Использование рассматривается на уровне визуальных приложений. |
| [LingBot-VLA](https://github.com/Robbyant/lingbot-vla), [LingBot-VLA v2](https://github.com/Robbyant/lingbot-vla-v2) | Исследования пространственных агентов и действий, после стабилизации сцены. |
| [VGGT](https://github.com/facebookresearch/vggt), [DUSt3R](https://github.com/naver/dust3r), [MASt3R](https://github.com/naver/mast3r) | Референсы поз, глубины и соответствий. Сравнивать с COLMAP на проблемных сценах. Проверять условия конкретных весов отдельно. |
| [gsplat](https://github.com/nerfstudio-project/gsplat), [Nerfstudio](https://github.com/nerfstudio-project/nerfstudio) | Производные визуальные представления. Для приёмки визуализации и для измерений используются отдельные метрики. |

Тестовые репозитории из [реестра наборов](../../DATASET_LICENSES.yml): [Replica](https://github.com/facebookresearch/Replica-Dataset) и [BlendedMVS](https://github.com/YoYo000/BlendedMVS). Сначала проверяем пригодность и условия конкретных данных. RealityCapture, Metashape, Pix4D, ContextCapture, PHOTOMOD, Cyclone, FARO Scene, Trimble RealWorks, Cupix, GoSLAM Studio и CHCNAV CoPre остаются продуктовыми референсами для сценариев и сравнений.

## Первый этап реализации — совмещение двух облаков по парам точек

**Пользовательский сценарий.** Одно помещение снято двумя способами. Photo Cloud строит PLY из фото. A2 создаёт LAS и траекторию, которые импортируются через PR #18. Оператор выбирает одинаковые физические точки на двух облаках. Для первой проверки используем шесть распределённых опор, в том числе на разной высоте, и две или больше независимых проверочных точки. Для инженерной приёмки заранее задаются требования к точности, системе координат и покрытию.

1. Начать с CLI в `python/research/photo_to_cloud`. Вход — исходный PLY из фото, сохранённая сессия A2 и таблица пар с уникальными ID, XYZ обеих сторон и ролями `fit` / `check`. Пары можно собрать текущим инструментом «Выбрать точку», который показывает исходный XYZ. Проверить, что выбранные координаты относятся к указанным файлам и что единицы и оси A2 заявлены пользователем.
2. Вычислить преобразование **A2 ← фото** в float64 по опорным парам. При неопределённом масштабе оценить положительный коэффициент вместе с вращением и переносом (Sim(3)). При независимо установленном масштабе оценивать вращение и перенос (SE(3)). Проверить конечность, распределение и вырожденность опор, знак определителя вращения и остатки. Перекосы и зеркальные координаты требуют явной диагностики.
3. Создать отдельный `alignment.json` с хешами исходников, направлением преобразования, масштабом, матрицей, системой координат, ошибками каждой пары и отдельными сводками fit/check. Оценка в метрах допустима при подтверждённых единицах опорного кадра. Без независимых точек статус проверки точности — `not_assessed`. Хеши и отчёт хранятся отдельно от исходников и манифеста A2.
4. Показать оба облака разными слоями в общей системе координат. Траектория остаётся в кадре A2, а матрица применяется к фотооблаку. После ручной приёмки можно выгрузить отдельную производную версию. Сначала проверяем воспроизводимость преобразования и визуальное совпадение общих поверхностей.
5. После получения базовой ошибки сравнить с ICP из [Open3D](https://www.open3d.org/docs/latest/tutorial/pipelines/icp_registration.html). Выполнять уточнение на подтверждённом перекрытии с уже заданным масштабом. Принимать только при улучшении независимой проверки и допустимом остатке. Отдельные тесты нужны для повторяющейся геометрии, малого перекрытия и неверной пары.

Первая приёмка включает синтетический набор с известным масштабом и крупным переносом, вырожденные и зеркальные соответствия, повторный запуск с теми же входами, затем реальную пару фото + A2. Пробный расчёт SVD на NumPy с шестью опорами и двумя независимыми парами восстановил заданный масштаб 1,7 и правильное вращение, остаток на независимых точках 4,37 × 10⁻¹¹ м в синтетическом примере. Это проверка численного подхода. Реальная точность определяется отдельным набором.

Для нескольких съёмок используем проверенные преобразования как ограничения между сессиями. Оптимизация траектории и пересборка накопленного LiDAR-облака требуют исходных кадров или подтверждённой связи точек с позами. Окрашивание облака из фотографий следует после регистрации и проверки поз, проекции и видимости.

## Источники внутри проекта

- [Реестр зависимостей](../../THIRD_PARTY.yml), [реестр весов](../../MODEL_LICENSES.yml), [инструкция Spatial Photo Cloud](../../python/research/photo_to_cloud/README.md).
- [P3 External Research Gate](../architecture/P3-external-research-gate.md), [аудит внешних реализаций](../architecture/P3-ARCH-REF-1-external-reference-audit.md), [проверка P3.1](../architecture/P3.1-reliability-verification-2026-10-27.md).
- [Документация импорта A2 на проверенном коммите](https://github.com/SystemDesignInstall/3ddotcloude/blob/755ecbe997e311bcc9960f06cf57e473f7eb9c10/docs/development/a2-mapper-import.md).
- Матрица SP-ARCH-MATRIX-0001, PHOTOGRAMMETRY-ARCHITECTURE-GAP-ANALYSIS-0001 и SPEC-360-CAPTURE-RECON-0001 из материалов проекта.
