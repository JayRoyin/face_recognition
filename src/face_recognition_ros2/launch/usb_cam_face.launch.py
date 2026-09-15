from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from launch.conditions import IfCondition
from launch_ros.parameter_descriptions import ParameterValue
import os

def generate_launch_description():
    # Project root: install/<pkg>/share/<pkg>/launch/usb_cam_face.launch.py
    # Navigate up 5 levels to reach project_root, then into models/
    project_root = os.path.abspath(
        os.path.join(os.path.dirname(__file__), '..', '..', '..', '..', '..'))
    models_dir = os.path.join(project_root, 'models')

    # ---- Args --------------------------------------------------------------
    detection_model_arg = DeclareLaunchArgument(
        'detection_model',
        default_value=os.path.join(models_dir, 'det_10g.onnx'),
        description='Face detection model (.onnx) — RetinaFace det_10g.onnx')
    recognition_model_arg = DeclareLaunchArgument(
        'recognition_model',
        default_value=os.path.join(models_dir, 'w600k_r50.onnx'),
        description='ArcFace recognition model (.onnx) — w600k_r50.onnx')

    image_topic_arg = DeclareLaunchArgument(
        'image_topic', default_value='/image_raw',
        description='Input image topic (usb_cam publishes here)')
    result_topic_arg = DeclareLaunchArgument(
        'result_topic', default_value='/face/recognition_result',
        description='Recognition result topic')
    annotated_topic_arg = DeclareLaunchArgument(
        'annotated_topic', default_value='/face/annotated',
        description='Annotated-image topic published by face_viewer_node')

    confidence_arg = DeclareLaunchArgument(
        'confidence_threshold', default_value='0.7',
        description='Recognition similarity threshold (0~1)')

    # usb_cam arguments (forwarded)
    video_device_arg = DeclareLaunchArgument(
        'video_device', default_value='/dev/video0',
        description='V4L2 video device')
    image_width_arg = DeclareLaunchArgument(
        'image_width', default_value='640')
    image_height_arg = DeclareLaunchArgument(
        'image_height', default_value='480')
    pixel_format_arg = DeclareLaunchArgument(
        'pixel_format', default_value='yuyv',
        description='V4L2 pixel format: yuyv | mjpeg | ...')
    framerate_arg = DeclareLaunchArgument(
        'framerate', default_value='30')

    show_window_arg = DeclareLaunchArgument(
        'show_window', default_value='false',
        description='DEPRECATED. face_viewer_node no longer opens an OpenCV '
                    'window. Use the face_stream_server (HTTP MJPEG) instead.')
    window_wait_ms_arg = DeclareLaunchArgument(
        'window_wait_ms', default_value='30',
        description='DEPRECATED.')

    # face_stream_server (browser-based MJPEG viewer)
    stream_port_arg = DeclareLaunchArgument(
        'stream_port', default_value='8090',
        description='HTTP port for face_stream_server. Set 0 to disable.')
    stream_image_topic_arg = DeclareLaunchArgument(
        'stream_image_topic', default_value=LaunchConfiguration('annotated_topic'),
        description='Image topic to serve over MJPEG.')
    jpeg_quality_arg = DeclareLaunchArgument(
        'jpeg_quality', default_value='80',
        description='JPEG quality (0-100) for the MJPEG stream.')

    # ---- Nodes -------------------------------------------------------------
    usb_cam_node = Node(
        package='usb_cam',
        executable='usb_cam_node_exe',
        name='usb_cam',
        output='screen',
        parameters=[{
            'camera_name':   'default_cam',
            'video_device':  LaunchConfiguration('video_device'),
            'image_width':   LaunchConfiguration('image_width'),
            'image_height':  LaunchConfiguration('image_height'),
            'pixel_format':  LaunchConfiguration('pixel_format'),
            # usb_cam declares framerate as double; declare it with the right
            # type so ros2 doesn't reject int-from-String-30 vs double.
            'framerate':     ParameterValue(LaunchConfiguration('framerate'), value_type=float),
            'brightness':    50,
            'contrast':      50,
            'saturation':    50,
            'sharpness':     50,
            'exposure_auto': 1,
            'focus_auto':    0,
        }]
    )

    face_recognition_node = Node(
        package='face_recognition_ros2',
        executable='face_recognition_node',
        name='face_recognition_node',
        output='screen',
        parameters=[{
            'detection_model':       LaunchConfiguration('detection_model'),
            'recognition_model':     LaunchConfiguration('recognition_model'),
            'image_topic':           LaunchConfiguration('image_topic'),
            'result_topic':          LaunchConfiguration('result_topic'),
            'confidence_threshold':  LaunchConfiguration('confidence_threshold'),
        }]
    )

    face_viewer_node = Node(
        package='face_recognition_ros2',
        executable='face_viewer_node',
        name='face_viewer_node',
        output='screen',
        parameters=[{
            'image_topic':      LaunchConfiguration('image_topic'),
            'result_topic':     LaunchConfiguration('result_topic'),
            'annotated_topic':  LaunchConfiguration('annotated_topic'),
        }]
    )

    face_stream_server_node = Node(
        package='face_recognition_ros2',
        executable='face_stream_server',
        name='face_stream_server',
        output='screen',
        condition=IfCondition(LaunchConfiguration('enable_stream_server')),
        parameters=[{
            'image_topic':  LaunchConfiguration('stream_image_topic'),
            'port':         LaunchConfiguration('stream_port'),
            'jpeg_quality': LaunchConfiguration('jpeg_quality'),
        }]
    )

    enable_stream_server_arg = DeclareLaunchArgument(
        'enable_stream_server', default_value='true',
        description='Start the HTTP MJPEG stream server (face_stream_server).')

    return LaunchDescription([
        # args
        detection_model_arg, recognition_model_arg,
        image_topic_arg, result_topic_arg, annotated_topic_arg,
        confidence_arg,
        video_device_arg, image_width_arg, image_height_arg,
        pixel_format_arg, framerate_arg,
        show_window_arg, window_wait_ms_arg,
        stream_port_arg, stream_image_topic_arg, jpeg_quality_arg,
        enable_stream_server_arg,
        # nodes
        usb_cam_node,
        face_recognition_node,
        face_viewer_node,
        face_stream_server_node,
    ])