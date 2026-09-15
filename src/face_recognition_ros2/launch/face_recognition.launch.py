from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from launch.conditions import IfCondition
import os

def generate_launch_description():
    # Path to models directory: install/<pkg>/share/<pkg>/launch/face_recognition.launch.py
    # Navigate up 5 levels to reach project_root, then into models/
    models_dir = os.path.join(os.path.dirname(__file__), '..', '..', '..', '..', '..', 'models')

    detection_model_arg = DeclareLaunchArgument(
        'detection_model',
        default_value=os.path.join(models_dir, 'det_10g.onnx'),
        description='Face detection model path (RetinaFace det_10g.onnx)'
    )

    recognition_model_arg = DeclareLaunchArgument(
        'recognition_model',
        default_value=os.path.join(models_dir, 'w600k_r50.onnx'),
        description='ArcFace recognition model path (.onnx)'
    )

    image_topic_arg = DeclareLaunchArgument(
        'image_topic',
        default_value='/image_raw',
        description='Input image topic'
    )

    result_topic_arg = DeclareLaunchArgument(
        'result_topic',
        default_value='/face/recognition_result',
        description='Output recognition result topic'
    )

    confidence_arg = DeclareLaunchArgument(
        'confidence_threshold',
        default_value='0.7',
        description='Recognition similarity threshold'
    )

    # Whether to also launch the face_viewer_node (UI window).
    launch_viewer_arg = DeclareLaunchArgument(
        'launch_viewer',
        default_value='true',
        description='Launch the GUI viewer (face_viewer_node) alongside the recognition node'
    )
    annotated_topic_arg = DeclareLaunchArgument(
        'annotated_topic',
        default_value='/face/annotated',
        description='Annotated image topic (published by face_viewer_node)'
    )
    show_window_arg = DeclareLaunchArgument(
        'show_window',
        default_value='false',
        description='DEPRECATED. face_viewer_node no longer opens a window.'
    )

    # HTTP stream server (replaces rqt_image_view).
    stream_port_arg = DeclareLaunchArgument(
        'stream_port', default_value='8090',
        description='HTTP port for the face_stream_server. Set 0 to disable.')
    enable_stream_server_arg = DeclareLaunchArgument(
        'enable_stream_server', default_value='true',
        description='Start the HTTP MJPEG stream server alongside everything.')

    node = Node(
        package='face_recognition_ros2',
        executable='face_recognition_node',
        name='face_recognition_node',
        output='screen',
        parameters=[{
            'detection_model': LaunchConfiguration('detection_model'),
            'recognition_model': LaunchConfiguration('recognition_model'),
            'image_topic': LaunchConfiguration('image_topic'),
            'result_topic': LaunchConfiguration('result_topic'),
            'confidence_threshold': LaunchConfiguration('confidence_threshold'),
        }]
    )

    viewer = Node(
        package='face_recognition_ros2',
        executable='face_viewer_node',
        name='face_viewer_node',
        output='screen',
        condition=IfCondition(LaunchConfiguration('launch_viewer')),
        parameters=[{
            'image_topic':     LaunchConfiguration('image_topic'),
            'result_topic':    LaunchConfiguration('result_topic'),
            'annotated_topic': LaunchConfiguration('annotated_topic'),
        }]
    )

    stream_server = Node(
        package='face_recognition_ros2',
        executable='face_stream_server',
        name='face_stream_server',
        output='screen',
        condition=IfCondition(LaunchConfiguration('enable_stream_server')),
        parameters=[{
            'image_topic': LaunchConfiguration('annotated_topic'),
            'port':        LaunchConfiguration('stream_port'),
        }]
    )

    return LaunchDescription([
        detection_model_arg,
        recognition_model_arg,
        image_topic_arg,
        result_topic_arg,
        confidence_arg,
        launch_viewer_arg,
        annotated_topic_arg,
        show_window_arg,
        stream_port_arg,
        enable_stream_server_arg,
        node,
        viewer,
        stream_server,
    ])