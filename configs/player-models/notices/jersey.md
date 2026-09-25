Hockey PARSeq (CC BY-NC 3.0; noncommercial)

Maria Koshkina and James H. Elder.
A General Framework for Jersey Number Recognition in Sports Video.
Proceedings of the IEEE/CVF Conference on Computer Vision and Pattern Recognition Workshops, June 2024, pages 3235-3244.
Project: https://github.com/mkoshkina/jersey-number-pipeline
Paper: https://openaccess.thecvf.com/content/CVPR2024W/CVsports/papers/Koshkina_A_General_Framework_for_Jersey_Number_Recognition_in_Sports_Video_CVPRW_2024_paper.pdf
PARSeq / Scene Text Recognition Model Hub: Copyright 2022 Darwin Bautista.
Original PARSeq project: https://github.com/baudm/parseq

Hockey model: Creative Commons Attribution-NonCommercial 3.0 Unported (CC BY-NC 3.0). https://creativecommons.org/licenses/by-nc/3.0/
Use and redistribution remain subject to its noncommercial and attribution conditions. Conversion and automatic download do not remove these conditions. The Apache-2.0 PARSeq implementation does not relicense the hockey checkpoint. No author endorsement is implied.

Conversion:
Converted the author-provided hockey-finetuned checkpoint to ONNX opset 17 with dynamic batch. Kept the full 95-class output vocabulary, autoregressive decoding, and one refinement pass. Limited inference to two characters plus EOS. Selected the known singleton token axis explicitly and used the mathematically equivalent explicit encoder attention branch for TensorRT 10.3 compatibility. No new training or fine-tuning.

Original checkpoint:
https://drive.google.com/file/d/1FyM31xvSXFRusN0sZH0EWXoHwDfB9WIE/view
SHA256: 5a31899866c092ff26b245898d1fb16ecaabcd091afaadeb6a9c056ca6c846e3
Configuration SHA256: b1c1374a8e672b0929c1eb26ed0eee4da199bacec84602033c018d15357e400c

Portable ONNX:
parseq-hockey-cvprw2024-df48ad7463aa89a5.onnx
SHA256: df48ad7463aa89a5e852f9c4cf2616f0cb9a4e7490ef380b0c6ab67f110228b1
